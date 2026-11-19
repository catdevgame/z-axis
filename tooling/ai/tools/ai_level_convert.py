#!/usr/bin/env python3
"""A glTF model with named objects into Aggressive Inline's level files (ZGE3),
through ai_level_write.py's writers, with the PS2 budgets checked at
conversion: textures made to fit (and the report says what was done), meshes
cut the way the exporter cut them, and the rest reported against the stock
levels' own maxima.

    python3 -I ai_level_convert.py convert MODEL.gltf --out DIR [--slot TUT] [--level-name NAME] [--sky SKY.png]
                                      DIR/ASSETS/LEVELS/<slot>/<slot>.{ZIT,GEO,INS,TXR,DAT}, <slot>.BIN, <slot>T.BIN,
                                      MOTIONS.ZAL: a Mods folder's layout, served in that level's slot
    python3 -I ai_level_convert.py info MODEL.gltf      what the converter sees and the budgets, nothing written
    python3 -I ai_level_convert.py example --out DIR     the small test park (park.gltf, park.bin, its textures) and
                                                        a sky for --sky (sky.png), made here from numbers alone
    python3 -I ai_level_convert.py selftest              the example converted, read back and checked

The model (glTF 2.0, as Blender exports it; gltf_model.py reads it):

  Axes: glTF is +y up, right-handed; the game's world is +y down. A point (x, y, z) becomes (x, -y, -z): a
  half turn about x, so handedness, windings and normals carry over unchanged.

  Tags are `#word` or `#key=value` suffixes on a node's, mesh's or material's name ("ledge#grind#metal") or
  the same keys in its extras (Blender's custom properties export as extras).

  A node with a triangle mesh is a level object placed by the node's transform (its scale baked into the
  vertices; one object per mesh and scale, instanced per node). It collides unless tagged #deco; #hidden collides
  but is not drawn; #sky is drawn as the sky (a panorama around the origin, as the stock levels'). An object
  over FACES_PER_OBJECT faces is cut into pieces (`name`, `name_2`, ...) at the median of its faces, as the
  exporter cut the stock levels (no stock part has more than 1,023 faces). An object is named by its mesh: the
  file keeps the name's first 31 characters and the hash of the whole name (W.GeoHeader), the word the game finds
  objects by (findWorldObjectByHash, cloneDynamicsDefinition); a mesh's extras "name_hash" gives that word instead
  (zge_level_gltf's export of a stock object whose name the field had cut).

  #dynamics on a node makes its object a dynamics prop, knocked about by the rider (DynamicsDef): the mesh's extras
  "dynamics" hold its definition (ZDYN's bodies and links, as zge_level_gltf.py exports a stock prop), written after
  the objects with the object's name hash as its DDEF; the object gets DynamicsObject's type word with the game's
  class data and its instances the game's prop flags and tags (DynamicsForm). One prop is one object: a mesh the
  converter would cut is an error.

  A node with a line mesh marks grind edges: every face edge of a collidable object that lies along one of
  its segments gets an edge record. #grind (a ledge or rail: kind 0), #grind=lip (a quarter pipe's coping,
  kind 1), #grind=spine (2), #grind=vpole (a vertical pole, 3), #grind=hpole (a horizontal pole, 4); the
  sound by #metal (2), #wood (1), default concrete (0).

  A material's tag gives its faces a surface kind: #qp (a quarter pipe, kind 1), #spine (2), #slope=G (a
  slope with gravity G, 3), #wall (a wall ride, 8); #2sided draws and collides both sides, #2sided=collide
  collides both sides and is drawn on one (most stock faces); #layer=<32 hex digits> takes that MATI layer state's
  blending and alpha, destination-alpha and Z tests (layer_state: a stock material's light shaft or puddle). Its base colour
  texture (a PNG) is the level texture; without one the base colour is an 8 x 8 texture of its own.

  An empty node #spawn (or #spawn=PlayerOne; the name without tags otherwise) is a spawn point with the
  node's position and facing; PlayerOne is where the skater starts, PlayerTwo the second player's start in split
  screen, and #spawn=respawn a point a fallen player is put back at (AI's "Respawn", DM2's "Player", BMX XXX's
  unnamed spawns: each converter writes its game's name). An empty node #camera (#fov=N degrees, 60 by default) is
  an idle camera: its position the eye, its rotation the view (+z forward in the games' axes, as BMX XXX's
  "CameraPos" points). Each converter writes them as "CameraPos" points named "idle", numbered from 1 in the
  model's order: BMX XXX looks from them behind its play-again screen, DM2 (with its level camera points option)
  in its level-point camera; AI reads none and keeps them so its map carries them to the other games. A level with
  no camera gets one where the game reads them (MAKES_CAMERA): behind and above its PlayerOne spawn, looking at it. An empty node #zone is a visibility
  zone: its box is the node's position +- its scale (Blender's empty cube of size 1), or its mesh's bounds
  when it has one; #sees=a,b lists the zones visible from it, every other zone is hidden there.

Budgets (the numbers behind them: docs/CUSTOM-LEVELS.md section 2):
  texels          TEXEL_BUDGET_KB (2,560 KB) over the whole level, a byte a texel: over it, the largest
                  textures are halved first until it fits (the Zoo converter's rule); said in the report
  texture side    TEXTURE_SIDE (128): a larger texture is halved to it; a side that is not a power of two is
                  resampled down to one
  strip groups    at most StripGroup.GROUP_ENTRIES (60) entries each, the VU1 buffer's: cut by construction
  faces           FACES_PER_OBJECT (1,024) a collision object (the exporter's cut); the tree's u16 face indices
  vertices        VERTICES_PER_PART (16,384): a strip entry's 14 index bits
  parts in view   PARTS_IN_VIEW (1,083, the Civic Center's worst zone): the parts drawn from a zone (its own, the
                  zones it does not hide, and parts in no zone); over it the model needs more zones: an error
                  unless --force

Python 3.8+, standard library only.
"""
import argparse
import math
import os
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Tuple

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ai_level as L  # noqa: E402
import ai_level_write as W  # noqa: E402
import gltf_model as G  # noqa: E402
import zge_scene as Z  # noqa: E402

Chunk = L.Chunk

TEXEL_BUDGET_KB = 2560
TEXTURE_SIDE = 128
FACES_PER_OBJECT = 1024
VERTICES_PER_PART = 16384
PARTS_IN_VIEW = 1083
CELL = 50.0                 # TEHD: the collision world's cell, every stock level's
LOD_DISTANCE = 500.0        # TGLO: the stock parts' usual draw distance
NAME_BYTES = 31             # an object's, instance's or texture's name (32 with its NUL)

# The stock exporter's words, read from the files (ai_level_write.py's census and LOCAL): an instance that
# collides, one that does not; the one object class every terrain object has; a part's render class.
INSTANCE_FLAGS_COLLIDES = 0x63
INSTANCE_FLAGS_DECORATION = 0x61
INSTANCE_FLAG_SHOWN = 0x01        # the node's kNodeShown: off, the object collides but is not drawn (#hidden)
INSTANCE_FLAG_SKY = 0x10          # the node's kNodeSky: its parts are drawn as the sky (#sky; the stock panorama_* instances, 0x73)
TERRAIN = L.name_hash("Terrain")
DYNAMICS_OBJECT = L.name_hash("DynamicsObject")   # the level object type of a dynamics prop (its TGTD word)
PART_FLAGS = 1
# The sky's render class (#sky): every stock level's one panorama part has it. The class gives the part its render
# type when it loads (g_renderClassTypes: class 3 is 0x18, the sky program, which draws at Z = 1, behind everything);
# with the rigid class (1) the rigid program drew the sky node at the dome's own depth, unclipped, and on the CPU
# renderer it covered the level (sky2, 2026-11-16).
PART_FLAGS_SKY = 3
# LOCAL's one material's layer: triangle strips, Gouraud, no blending, wrap both ways, RGBA texture
# function modulate, mipmaps by LOD, Z test greater-or-equal (material_state.cpp kMatiFields).
OPAQUE_LAYER_STATE = (0x0000001C, 0x40000000, 0x49F782F0, 0x1C000101)
MATERIAL_COLOURS = bytes.fromhex("e5e5e5ff" "f3f3f3ff" "f3f3f3ff")

GRIND_KINDS = {"ledge": 0, "rail": 0, "lip": 1, "spine": 2, "vpole": 3, "hpole": 4}
GRIND_SOUNDS = {"concrete": 0, "wood": 1, "metal": 2}
SURFACE_KINDS = {"qp": 1, "spine": 2, "slope": 3, "wall": 8}
CAMERA_POINT = L.name_hash("CameraPos")   # a camera point's kind (BMX XXX's camera.c)
IDLE_CAMERA = L.name_hash("idle")         # the idle cameras' name (BMX XXX's idleCameraMode)
CAMERA_FOV = 60                           # an idle camera's field of view when the model gives none (the stock one)
START_CAMERA_BACK = 8.0                   # a made camera: this far behind the PlayerOne spawn (m)
START_CAMERA_UP = 3.0                     # and this far above it, looking at a point
START_CAMERA_LOOK_UP = 1.0                # this far above the spawn
QP_LAUNCH_PUSH = 4.0        # TUT's quarter pipes: FaceTag(0, 1, launch_push=4.0 or 5.0)


# ---- tags -----------------------------------------------------------------------------

def split_tags(name, extras=None):
    """("name", {tag: value}) from "name#tag#key=value" and the extras (which win)."""
    parts = name.split("#")
    tags = {}
    for t in parts[1:]:
        if not t:
            continue
        key, _, value = t.partition("=")
        tags[key.strip().lower()] = value.strip() if value else True
    for k, v in (extras or {}).items():
        tags[str(k).lower()] = v if not isinstance(v, str) or v else True
    return parts[0].strip(), tags


def to_ai(p):
    return (float(p[0]), -float(p[1]), -float(p[2]))


def quat_to_ai(q):
    return (float(q[0]), -float(q[1]), -float(q[2]), float(q[3]))


def short(name, used):
    """A distinct name of at most NAME_BYTES bytes. Distinct as the games tell names apart: their name hash
    (ai_level.name_hash, an id's) ignores case, so `used` holds the names upper-cased."""
    base = name[:NAME_BYTES] or "object"
    out, n = base, 2
    while out.upper() in used:
        tail = "_%d" % n
        out = base[:NAME_BYTES - len(tail)] + tail
        n += 1
    used.add(out.upper())
    return out


def distinct(name, used):
    """A name no other in `used` has (as short's, case-insensitively), its length kept: an object's whole name, which
    its header hashes and cuts (W.GeoHeader)."""
    base = name or "object"
    out, n = base, 2
    while out.upper() in used:
        out = "%s_%d" % (base, n)
        n += 1
    used.add(out.upper())
    return out


# ---- strips -----------------------------------------------------------------------------

def stripify(triangles):
    """Triangle strips (vertex index lists) covering the triangles, each triangle once with its winding: a
    greedy walk over shared edges. The strips decode (decode_strips) to the same triangle set."""
    by_edge = defaultdict(list)   # directed edge -> triangle indices with that edge
    for i, (a, b, c) in enumerate(triangles):
        for e in ((a, b), (b, c), (c, a)):
            by_edge[e].append(i)
    used = [False] * len(triangles)
    strips = []

    def take(edge):
        for i in by_edge.get(edge, ()):
            if not used[i]:
                return i
        return None

    for start in range(len(triangles)):
        if used[start]:
            continue
        used[start] = True
        a, b, c = triangles[start]
        # the start rotated so the strip can go on along the edge with an unused neighbour
        for rot in ((a, b, c), (b, c, a), (c, a, b)):
            if take((rot[2], rot[1])) is not None:
                a, b, c = rot
                break
        strip = [a, b, c]
        while True:
            k = len(strip) - 2          # the next triangle's index in the strip
            v, w = strip[-2], strip[-1]
            # triangle k is (v, w, x) when k is even, (w, v, x) when odd (the strip's alternating winding)
            i = take((v, w) if k % 2 == 0 else (w, v))
            if i is None:
                break
            used[i] = True
            t = triangles[i]
            x = next(q for q in t if q != v and q != w)
            strip.append(x)
        strips.append(strip)
    return strips


def decode_strips(strips):
    """The triangles a strip list draws, each as its cyclic winding's smallest rotation."""
    out = []
    for s in strips:
        for k in range(len(s) - 2):
            t = (s[k], s[k + 1], s[k + 2]) if k % 2 == 0 else (s[k + 1], s[k], s[k + 2])
            out.append(cyclic(t))
    return out


def cyclic(t):
    rots = [(t[0], t[1], t[2]), (t[1], t[2], t[0]), (t[2], t[0], t[1])]
    return min(rots)


def pack_strips(submesh, strips, max_entries=W.StripGroup.GROUP_ENTRIES):
    """The strips as TGFM groups, several strips a group (each restarting with its first two entries drawing
    nothing, as the stock groups do: most of the Tutorial's hold five or more), a strip longer than a group
    continued across groups as StripGroup.from_strip does."""
    groups, current = [], []

    def flush():
        if current:
            groups.append(W.StripGroup(submesh, 0, list(current)))
            current.clear()

    for strip in strips:
        entries = []
        for k, v in enumerate(strip):
            if k < 2 or strip[k - 2] == strip[k - 1] or strip[k - 1] == v or strip[k - 2] == v:
                entries.append(v | W.StripGroup.NO_TRIANGLE)
            else:
                entries.append(v | (W.StripGroup.FLIPPED if k & 1 else 0))
        if len(entries) > max_entries:
            flush()
            groups += W.StripGroup.from_strip(submesh, strip, 0, max_entries)
            continue
        if len(current) + len(entries) > max_entries:
            flush()
        current += entries
    flush()
    return groups


# ---- textures ---------------------------------------------------------------------------

CLEAR_ALPHA = 8           # a texel's alpha under this is clear
SOLID_ALPHA = 247         # over this, solid
CUT_OUT_PARTIAL = 10      # a texture with fewer than this percent of its texels neither is a cut-out
CUT_OUT_KEEP = 77         # a cut-out's halved texel this covered (30% of 255) stays solid


@dataclass
class Texture:
    name: str
    width: int
    height: int
    rows: list                        # (r, g, b, a) rows, top first
    notes: List[str] = field(default_factory=list)

    @property
    def texels(self):
        return self.width * self.height

    @property
    def has_alpha(self):
        return any(px[3] < 255 for row in self.rows for px in row)

    @property
    def cut_out(self):
        """A cut-out (a fence's or railing's bars, leaves): clear texels, and under CUT_OUT_PARTIAL percent of them
        neither clear nor solid."""
        clear = partial = 0
        for row in self.rows:
            for px in row:
                if px[3] < CLEAR_ALPHA:
                    clear += 1
                elif px[3] <= SOLID_ALPHA:
                    partial += 1
        return clear > 0 and partial * 100 < CUT_OUT_PARTIAL * self.texels

    def halve(self):
        """Half the size each way, a 2 x 2 box filter. A cut-out stays one: a texel CUT_OUT_KEEP covered or more is
        solid, the rest clear, its colour the solid texels' (averaged as they are, one-texel bars came out half
        transparent and darkened by the clear texels' colour, and the blended material drew them as a faint ghost:
        a converted map's arches and railings, 2026-11-17)."""
        cut = self.cut_out
        w, h = self.width // 2, self.height // 2
        rows = []
        for y in range(h):
            r0, r1 = self.rows[2 * y], self.rows[2 * y + 1]
            row = []
            for x in range(w):
                four = (r0[2 * x], r0[2 * x + 1], r1[2 * x], r1[2 * x + 1])
                px = tuple((sum(p[c] for p in four) + 2) // 4 for c in range(4))
                if cut:
                    weight = sum(p[3] for p in four)
                    if weight:
                        px = tuple((sum(p[c] * p[3] for p in four) + weight // 2) // weight for c in range(3)) + (px[3],)
                    px = px[:3] + (255 if px[3] >= CUT_OUT_KEEP else 0,)
                row.append(px)
            rows.append(row)
        self.width, self.height, self.rows = w, h, rows

    def resample(self, w, h):
        """Nearest-neighbour to w x h (for sides that are not powers of two)."""
        rows = [[self.rows[y * self.height // h][x * self.width // w] for x in range(w)] for y in range(h)]
        self.width, self.height, self.rows = w, h, rows


def pow2_below(n):
    p = 1
    while p * 2 <= n:
        p *= 2
    return p


def fit_textures(textures, report, side=TEXTURE_SIDE, budget_kb=TEXEL_BUDGET_KB):
    """Every texture within `side` (halved, or resampled to a power of two first), then the whole set
    within the texel budget, the largest halved first. Each change said in the report."""
    for t in textures:
        w, h = pow2_below(t.width), pow2_below(t.height)
        if (w, h) != (t.width, t.height):
            report.did("texture %s %dx%d resampled to %dx%d: sides must be powers of two" % (t.name, t.width, t.height, w, h))
            t.resample(w, h)
        before = (t.width, t.height)
        while t.width > side or t.height > side:
            if min(t.width, t.height) < 2:
                break
            t.halve()
        if before != (t.width, t.height):
            report.did("texture %s %dx%d halved to %dx%d: the texture side budget (%d)" % (t.name, before[0], before[1], t.width, t.height, side))
    while sum(t.texels for t in textures) > budget_kb * 1024:
        big = max(textures, key=lambda t: t.texels)
        if big.texels <= 64:
            report.did("the texel budget (%d KB) cannot be met: %d textures of at most 8x8" % (budget_kb, len(textures)))
            break
        before = (big.width, big.height)
        big.halve()
        report.did("texture %s %dx%d halved to %dx%d: the level's texels over %d KB" % (big.name, before[0], before[1], big.width, big.height, budget_kb))


def palettize(rows, with_alpha):
    """(palette of at most 256 (r, g, b, a), index rows): the distinct colours as they are when they fit,
    else a median cut over them (weighted by use), each pixel to its nearest entry."""
    counts = Counter(px if with_alpha else px[:3] + (255,) for row in rows for px in row)
    colours = list(counts)
    if len(colours) <= 256:
        palette = colours
    else:
        boxes = [colours]
        while len(boxes) < 256:
            # the box with the widest channel range splits at its weighted median
            best, width, axis = None, -1, 0
            for b in boxes:
                if len(b) < 2:
                    continue
                for c in range(4 if with_alpha else 3):
                    lo, hi = min(px[c] for px in b), max(px[c] for px in b)
                    if hi - lo > width:
                        best, width, axis = b, hi - lo, c
            if best is None:
                break
            best.sort(key=lambda px: px[axis])
            total, acc, cut = sum(counts[px] for px in best), 0, 0
            for i, px in enumerate(best):
                acc += counts[px]
                if acc * 2 >= total:
                    cut = max(1, min(len(best) - 1, i + 1))
                    break
            boxes.remove(best)
            boxes += [best[:cut], best[cut:]]
        palette = []
        for b in boxes:
            n = sum(counts[px] for px in b)
            palette.append(tuple(int(round(sum(px[c] * counts[px] for px in b) / n)) for c in range(4)))
    nearest = {}
    for px in colours:
        if px in nearest:
            continue
        nearest[px] = min(range(len(palette)), key=lambda i: sum((palette[i][c] - px[c]) ** 2 for c in range(4)))
    index_rows = [[nearest[px if with_alpha else px[:3] + (255,)] for px in row] for row in rows]
    return palette, index_rows


def bmp8(width, height, palette, index_rows, with_alpha):
    """A Windows BMP of 8 bits a pixel as the PS2 levels' TBMP/TZMP chunks hold them: a 40-byte info
    header, a 256-colour BGRA palette (alpha 0 in a TBMP), rows bottom-up, the whole padded to 4 bytes
    and that length in the file header."""
    row_bytes = (width + 3) & ~3
    offset = 14 + 40 + 1024
    body = bytearray()
    body += struct.pack("<IiiHHIIiiII", 40, width, height, 1, 8, 0, 0, 2834, 2834, 0, 0)
    for i in range(256):
        r, g, b, a = palette[i] if i < len(palette) else (0, 0, 0, 255)
        body += struct.pack("<4B", b, g, r, a if with_alpha else 0)
    for y in range(height - 1, -1, -1):
        body += bytes(index_rows[y]).ljust(row_bytes, b"\0")
    total = (offset + len(body) - 54 + 3) & ~3
    out = b"BM" + struct.pack("<IHHI", total, 0, 0, offset) + bytes(body)
    return out.ljust(total, b"\0")


def texture_chunk(t):
    palette, indices = palettize(t.rows, t.has_alpha)
    header = struct.pack("<IHHHH8s", L.name_hash(t.name), t.width, t.height, 0, 0, b"\0" * 8) + W.c_field(t.name, 32)
    return Chunk(b"TXRC", kids=[Chunk(b"TXRH", header), Chunk(b"TZMP" if t.has_alpha else b"TBMP", bmp8(t.width, t.height, palette, indices, t.has_alpha))])


def write_txr(textures):
    return L.serialize([Chunk(b"FORM", kids=[texture_chunk(t) for t in textures])])


# ---- materials ---------------------------------------------------------------------------

def layer_state(base, given=None):
    """A MATI layer's four state words (material_state.cpp kMatiFields: the GS's own fields, read the same in all
    three games): `base`, with the blending and the tests of `given` (16 bytes, a #layer tag's) when there is one:
    word 0's ABE (bit 5), word 1's TCC (bit 30), word 2's TFX (bits 0-1) and ALPHA's A, B and C (bits 25-30), all of
    word 3 (ALPHA's D and FIX, the alpha, destination-alpha and Z tests). The rest (primitive, wrap and clamp, the
    texture's mipmaps and LOD) stays the base's: those depend on the texture the converter writes, one level."""
    words = list(base)
    if given is None:
        return tuple(words)
    g = struct.unpack("<4I", given)
    words[0] = (words[0] & ~0x20) | (g[0] & 0x20)
    words[1] = (words[1] & ~(1 << 30)) | (g[1] & (1 << 30))
    keep2 = 0x3 | (0x3F << 25)
    words[2] = (words[2] & ~keep2) | (g[2] & keep2)
    words[3] = g[3]
    return tuple(words)


def layer_of(tags):
    """A material's #layer=<32 hex digits> (zge_level_gltf.py writes the stock material's first layer) as bytes."""
    value = tags.get("layer")
    if not isinstance(value, str):
        return None
    try:
        data = bytes.fromhex(value)
    except ValueError:
        return None
    return data if len(data) == 16 else None


def mati(name, texture_name, two_sided=False, layer=None):
    """A MATI record (0xA0 bytes; zge::MatiChunk) with one layer of LOCAL's opaque state showing the texture (with a
    #layer's blending and tests: layer_state)."""
    flags = 0x2 if two_sided else 0
    out = struct.pack("<II", flags, L.name_hash(name)) + MATERIAL_COLOURS + struct.pack("<2f", 0.1, 1.0) + b"\0" * 16 + struct.pack("<I", 1)
    out += struct.pack("<4I", *layer_state(OPAQUE_LAYER_STATE, layer)) + struct.pack("<3I", L.name_hash(texture_name), 0, 0)
    return Chunk(b"MATI", out + b"\0" * (28 * 3))


# ---- the conversion ----------------------------------------------------------------------

class Report:
    def __init__(self):
        self.done = []       # what the converter changed to fit
        self.lines = []      # the level's numbers
        self.errors = []

    def did(self, text):
        self.done.append(text)

    def say(self, text):
        self.lines.append(text)

    def error(self, text):
        self.errors.append(text)

    def text(self):
        out = list(self.lines)
        if self.done:
            out.append("done to fit:")
            out += ["  " + d for d in self.done]
        if self.errors:
            out.append("over budget:")
            out += ["  " + e for e in self.errors]
        return "\n".join(out)


@dataclass
class Surface:
    """One material as the level sees it: its name, texture, surface kind and sides."""
    name: str
    texture: str
    kind: int = 0
    gravity: float = 0.0
    two_sided: bool = False         # #2sided: drawn (the MATI's flag) and collided on both sides
    faces_two_sided: bool = False   # #2sided=collide: collided on both sides (the faces' flag), drawn on one, as most
                                    # stock faces in all three games are (a stock level's MATIs rarely draw both sides)
    base_colour: Tuple[float, float, float, float] = (1.0, 1.0, 1.0, 1.0)
    layer: Optional[bytes] = None   # #layer=<32 hex digits>: a layer state whose blending and tests the material takes


@dataclass
class Piece:
    """A level object to be: vertices in object space (game axes), faces with their surface, and the tags."""
    name: str
    positions: List[Tuple[float, float, float]]
    normals: List[Tuple[float, float, float]]
    uvs: List[Tuple[float, float]]
    colours: List[Tuple[int, int, int, int]]
    faces: List[Tuple[int, int, int, int]]      # (surface index, a, b, c)
    collides: bool
    sky: bool = False                           # #sky: its part has the sky's render class
    name_hash: Optional[int] = None             # the object's name hash when its name was cut (the mesh's extras
                                                # name_hash: zge_level_gltf's export of a stock object); None: the name's
    dynamics: Optional["DynamicsDef"] = None    # #dynamics: a dynamics prop's definition


# ---- dynamics props -----------------------------------------------------------------------------------------
#
# A dynamics prop (a trash can, a cone, a chair) is a level object of the type DynamicsObject: its object's TGTD word
# is that type's hash, and at its set-up (initDynamicsLevelObject; BMX XXX's initDynamicsObject) the game takes the
# definition whose DDEF hash is the object's name hash (cloneDynamicsDefinition: its TGHD +0x3C) from the GEO's ZDYN
# chunks and knocks the prop about as its bodies. The ZDYN form is the same in all three games: DDEF (the name hash),
# then DBDY bodies (shape, body hash, mass, box, spheres, half extents; lengths in cm) and DCON links. What differs is
# the class data after the TGTD word and the instance's flags and tags (DynamicsForm).

@dataclass
class DynamicsDef:
    """#dynamics on a node: its object is a dynamics prop. From the mesh's extras "dynamics" (zge_level_gltf.py's
    export of a stock prop): {"game": the game the data is from, "class": the TGTD bytes after the type word,
    "instance_tags": the instance's tag bytes, "zdyn": [[chunk tag, data], ...] the definition after its DDEF},
    the bytes in hex."""
    chunks: List[Tuple[bytes, bytes]]
    game: Optional[str] = None
    class_data: bytes = b""
    instance_tags: bytes = b""

    @staticmethod
    def from_extras(value, where):
        if not isinstance(value, dict) or not value.get("zdyn"):
            raise ValueError("%s: #dynamics needs its definition, the mesh's extras \"dynamics\" with its \"zdyn\" chunks "
                             "(zge_level_gltf.py exports a stock prop so)" % where)
        chunks = [(str(tag).encode("latin1"), bytes.fromhex(data)) for tag, data in value["zdyn"]]
        if not any(tag == b"DBDY" for tag, _ in chunks):
            raise ValueError("%s: the dynamics definition has no body (DBDY)" % where)
        return DynamicsDef(chunks, value.get("game"), bytes.fromhex(value.get("class", "")), bytes.fromhex(value.get("instance_tags", "")))

    def to_extras(self):
        return {"game": self.game, "class": self.class_data.hex(), "instance_tags": self.instance_tags.hex(),
                "zdyn": [[tag.decode("latin1"), data.hex()] for tag, data in self.chunks]}

    def zdyn(self, name_hash):
        """The definition's ZDYN, named by the object's name hash (what the game's lookup compares)."""
        return Chunk(b"ZDYN", kids=[Chunk(b"DDEF", struct.pack("<I", name_hash))] + [Chunk(tag, data) for tag, data in self.chunks])


@dataclass(frozen=True)
class DynamicsForm:
    """What one game writes for a dynamics prop besides its geometry and ZDYN: the TGTD bytes after the type word, the
    instance's flags and tags. A definition from the same game keeps its own class data and tags; one from another
    game takes these (the games' sounds are their own: AI's sample numbers, DM2's object sound, BMX XXX's names)."""
    game: str
    instance_flags: int      # the stock props' (AI's and BMX XXX's 0x63, DM2's 0x23)
    class_data: bytes
    instance_tags: bytes


# AI: the class's tag bytes 4 and 5 are the first and later impact sounds (createDynamicsObject: + 0x40 in the
# level's bank); Civic Center's DynamicsDef_BoxA's, 1 and 1 (its other two bytes are not read: zero here).
AI_DYNAMICS = DynamicsForm("ai", 0x63, bytes([1, 1, 0, 0]), b"\0" * 4)


@dataclass
class Placement:
    node_name: str
    piece_key: tuple
    position: Tuple[float, float, float]
    rotation: Tuple[float, float, float, float]
    collides: bool
    zone: Optional[str] = None
    hidden: bool = False      # #hidden: collides but is not drawn (the node's shown bit off)
    sky: bool = False         # #sky: drawn as the sky (the node's sky bit, as a stock panorama_* instance)
    dynamics: bool = False    # #dynamics: a dynamics prop (DynamicsForm's flags and tags)


@dataclass
class EdgeMark:
    a: Tuple[float, float, float]      # world, game axes
    b: Tuple[float, float, float]
    kind: int
    flags: int
    sound: int


@dataclass
class Zone:
    name: str
    low: Tuple[float, float, float]
    high: Tuple[float, float, float]
    sees: List[str]


# ---- the sky ----------------------------------------------------------------------------------------------
#
# The three games draw a level's sky the same way: one instance with the sky bit (kNodeSky) is the world's sky node
# (addWorldNode: setSkyNode), drawSky moves it to the camera every frame and the sky program draws it at Z = 1,
# behind everything; the level's Globals scene point gives the clear colour and the fog, which should agree with
# it. --sky makes that from a picture: add_sky's dome (one model part, the same in every game) and its colours
# (SkyLook); what each game writes for them is its SkyForm, a record on its converter (Converter.SKY): the sky
# part's TGPH flags, the instance's flags and the Globals point's tag record with the game's fog.

@dataclass
class SkyLook:
    """What a sky picture gives the level: its zenith's colour (the clear colour, where no sky is drawn) and its
    horizon's (the fog's, so the sky and what the fog hides agree)."""
    zenith: Tuple[int, int, int]
    horizon: Tuple[int, int, int]


@dataclass
class Globals:
    """A level's Globals point in no game's layout: the clear colour, the fog's colour, alpha, start and end (m),
    the far clip and the light colour (DM2's and BMX XXX's first word, the scene light's or ambient; AI's has none)."""
    clear: Tuple[int, int, int]
    fog: Tuple[int, int, int]
    fog_alpha: int
    fog_start: float
    fog_end: float
    far_clip: float
    light: Tuple[int, int, int] = (0x80, 0x80, 0x80)


@dataclass(frozen=True)
class SkyForm:
    """How one game draws a converted level's sky and reads its Globals point."""
    part_flags: int          # the sky part's TGPH flags (its render class)
    instance_flags: int      # the sky instance's TGIH flags (with the sky bit)
    fog_alpha: int           # the fog's alpha as the game's stock skies have it
    fog_start: float
    fog_end: float
    far_clip: float
    tags: Callable[["Globals"], bytes]   # the Globals point's tag record, as the game's readers take it

    def globals_for(self, look):
        return Globals(clear=look.zenith, fog=look.horizon, fog_alpha=self.fog_alpha, fog_start=self.fog_start,
                       fog_end=self.fog_end, far_clip=self.far_clip)


def ai_globals_tags(g):
    """AI's Globals record (aggressive/camera.cpp applyLevelCameraSettings reads it at offsets 4 clear colour, 8 fog
    colour, 12 fog alpha as a float, 16 fog start, 20 fog end, 24 far clip). The record is the chunk's tag data
    itself (offset 0 unused): a record shifted 4 bytes up left the far clip past the record, and the game clipped
    everything (2026-11-16)."""
    return struct.pack("<I4B4Bffff", 0, *g.clear, 0x80, *g.fog, g.fog_alpha, float(g.fog_alpha), g.fog_start, g.fog_end, g.far_clip)


# AI's sky: the panorama part's render class 3 (every stock level has exactly one; g_renderClassTypes gives class 3
# the sky program's type 0x18 at load, and resolveRenderType never re-resolves a type already set: with the rigid
# class the rigid program drew the dome at its own depth, unclipped, and on the CPU renderer it covered the level,
# sky2 2026-11-16); the instance a decoration with the sky bit. The fog: eight planes from the start to the end,
# each blended at half the fog alpha over whatever is behind them, the sky included: the stock levels' alpha is 18
# to 25 (Civic Center's and Movie Lot's 24), at which the sky keeps 45% of its colour (at 96 it kept 2% and was the
# fog's colour); 240 to 360 m and the far clip 1000 as measured on a converted level.
AI_SKY = SkyForm(part_flags=PART_FLAGS_SKY, instance_flags=INSTANCE_FLAGS_DECORATION | INSTANCE_FLAG_SKY, fog_alpha=24,
                 fog_start=240.0, fog_end=360.0, far_clip=1000.0, tags=ai_globals_tags)
SKY_RADIUS = 28.0           # Civic Center's panorama ring


def add_sky(model, png_path, radius=SKY_RADIUS):
    """A sky for the level, drawn as the stock levels draw theirs: a dome flagged #sky (the game moves it to the
    camera each frame: drawSky), its inside faces textured with the PNG (u repeated four times around, v from the
    zenith down to 30 degrees below the horizon), vertex colour 1.5 (over the texture, as the stock levels light).
    The sky program draws it at Z = 1, behind everything, whatever its radius (28 m and 160 m domes drew the same
    picture in AI, sky2 2026-11-16); 28 m, Civic Center's ring, keeps the level's world box (it counts the sky's box
    at the level's centre) the level's own. The picture's look (SkyLook) is returned for the Globals point."""
    with open(png_path, "rb") as f:
        width, height, rows = G.read_png(f.read())
    model.images.append(G.Image("sky", width, height, rows))
    model.materials.append(G.Material("sky", texture=len(model.images) - 1))
    lats, segs = [-30, -12, 5, 20, 38, 60, 90], 20
    positions, uvs, triangles = [], [], []
    for lat in lats:
        v = (90 - lat) / 120.0
        for s in range(segs + 1):
            a = 2 * math.pi * s / segs
            cl, sl = math.cos(math.radians(lat)), math.sin(math.radians(lat))
            positions.append((radius * cl * math.cos(a), radius * sl, radius * cl * math.sin(a)))
            uvs.append((4.0 * s / segs, v))
    for li in range(len(lats) - 1):
        for s in range(segs):
            a = li * (segs + 1) + s
            b, c, d = a + 1, a + segs + 1, a + segs + 2
            # inside faces: wound so that the normal points at the centre
            for tri in ((a, b, c), (b, d, c)):
                p0, p1, p2 = (positions[i] for i in tri)
                n = (((p1[1] - p0[1]) * (p2[2] - p0[2]) - (p1[2] - p0[2]) * (p2[1] - p0[1])),
                     ((p1[2] - p0[2]) * (p2[0] - p0[0]) - (p1[0] - p0[0]) * (p2[2] - p0[2])),
                     ((p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0])))
                centre = tuple((p0[k] + p1[k] + p2[k]) / 3 for k in range(3))
                triangles.append(tri[::-1] if sum(n[k] * centre[k] for k in range(3)) > 0 else tri)
    prim = G.Primitive(positions, triangles, uvs=uvs, colours=[(1.5, 1.5, 1.5, 1.0)] * len(positions), material=len(model.materials) - 1)
    model.meshes.append(G.Mesh("panorama", [prim]))
    identity = [1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
    model.nodes.append(G.Node("panorama#sky", (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0), identity, mesh=len(model.meshes) - 1))

    def mean(rs):
        px = [p for r in rs for p in r]
        return tuple(int(sum(p[k] for p in px) / len(px)) for k in range(3))
    return SkyLook(zenith=mean(rows[:max(1, height // 8)]), horizon=mean(rows[-max(1, height // 8):]))


def add_sky_arguments(parser):
    """--sky and --sky-radius, the same in the three games' converters."""
    parser.add_argument("--sky", help="a PNG drawn as the level's sky (a dome the game keeps around the camera, as the stock panoramas), "
                                      "with the level's clear colour and fog from its zenith and horizon")
    parser.add_argument("--sky-radius", type=float, default=SKY_RADIUS,
                        help="the sky dome's radius (drawn behind everything at any radius; its box counts in the level's world box)")


def look_rotation(d):
    """The rotation (a quaternion x, y, z, w, the games' axes) that turns +z to the unit direction d with no roll: a
    pitch about x, then a yaw about y. The half angles' cosines and sines from d by square roots alone, so the
    native converter (collection/levels) gets the same bits."""
    h = math.sqrt(d[0] * d[0] + d[2] * d[2])
    cos_yaw = d[2] / h
    cy = math.sqrt(max(0.0, (1.0 + cos_yaw) / 2.0))
    sy = math.copysign(math.sqrt(max(0.0, (1.0 - cos_yaw) / 2.0)), d[0])
    cx = math.sqrt(max(0.0, (1.0 + h) / 2.0))
    sx = math.copysign(math.sqrt(max(0.0, (1.0 - h) / 2.0)), -d[1])
    return (cy * sx, sy * cx, -sy * sx, cy * cx)


class Camera:
    """An idle camera: the eye (the games' axes), the view's rotation (+z forward) and the field of view (degrees)."""
    def __init__(self, position, rotation, fov=CAMERA_FOV):
        self.position, self.rotation, self.fov = position, rotation, fov


class Converter:
    """The model into a level. A game's converter (dm2_level_convert.py) is a subclass: its record module (RECORDS,
    the game's Geometry, Instance, Instances), its budgets and its own geo_object, tag_edges, scene_data,
    texture_file and scene_file."""
    RECORDS = W
    TEXEL_BUDGET_KB = TEXEL_BUDGET_KB
    TEXTURE_SIDE = TEXTURE_SIDE
    PARTS_IN_VIEW = PARTS_IN_VIEW
    PARTS_IN_VIEW_SOURCE = "the Civic Center's worst zone"
    # The level files' bytes (GEO, INS, DAT, TXR): the game reads them whole into its 32 MB heap, so a level no larger
    # than the game's largest stock one fits as that one does. Over it a level is refused, not written: DM2's park slot
    # with Woodward's 17.3 MB of files ran its heap out (29.1 MB in use) and the load never ended (freeplay4).
    LEVEL_BYTES = 10577384
    LEVEL_BYTES_SOURCE = "the Museum's, AI's largest stock level"
    FIRST_OBJECT_ID = 1     # objects take odd ids from here, each part the id after its object's
    SKY = AI_SKY            # how the game draws the sky and reads its Globals point (SkyForm)
    DYNAMICS = AI_DYNAMICS  # what the game writes for a dynamics prop (DynamicsForm)
    # The spawns' names in the game (the model's #spawn=respawn is a game's own name; None: left out) and whether a
    # level with no idle camera gets one at its start (AI reads none). CAMERA_LIFT: what the game adds to a camera
    # point's position for the eye (DM2's level-point camera stands 5 m above its point).
    SPAWN_NAMES = {"playerone": "PlayerOne", "playertwo": "PlayerTwo", "respawn": "Respawn"}
    MAKES_CAMERA = False
    CAMERA_LIFT = 0.0
    SPAWN_TAGS = b""        # the tag record each spawn point gets

    def __init__(self, model, slot="TUT", level_name=None, force=False):
        self.model = model
        self.slot = slot.upper()
        self.level_name = level_name or slot
        self.force = force
        self.report = Report()
        self.surfaces: List[Surface] = []
        self.textures: Dict[str, Texture] = {}
        self.pieces: Dict[tuple, List[Piece]] = {}
        self.placements: List[Placement] = []
        self.edge_marks: List[EdgeMark] = []
        self.spawns = []
        self.cameras: List[Camera] = []
        self.zones: List[Zone] = []
        self.names = set()
        self.globals: Optional[Globals] = None   # the level's Globals point (use_sky)

    def use_sky(self, png_path, radius=None):
        """The level gets the picture as its sky (add_sky's dome) and the Globals point that matches it, in this
        game's form (SKY)."""
        look = add_sky(self.model, png_path, SKY_RADIUS if radius is None else radius)
        self.use_sky_look(look)
        return look

    def use_sky_look(self, look):
        """The level's Globals point for a sky of that look, in this game's form (SKY)."""
        self.globals = self.SKY.globals_for(look)

    def sky_look_of(self, pieces):
        """The look of a sky the model has as its own (a #sky node: a stock level's panorama exported by
        zge_level_gltf.py, or one made in Blender): what it shows at its vertices (the texel at each corner's UV
        under the vertex colour, 0x80 the texture as it is), the zenith's the mean over its top quarter by height,
        the horizon's over the quarter above its centre's height."""
        corners = []    # (height, (r, g, b))
        for piece in pieces:
            for s, *vs in piece.faces:
                t = self.textures[self.surfaces[s].texture]
                for v in vs:
                    u, w = piece.uvs[v]
                    x, y = int(math.floor(u * t.width)) % t.width, int(math.floor(w * t.height)) % t.height
                    texel = t.rows[y][x]
                    shade = piece.colours[v]
                    corners.append((-piece.positions[v][1], tuple(min(255, texel[k] * shade[k] // 0x80) for k in range(3))))
        if not corners:
            return None
        low, high = min(h for h, _ in corners), max(h for h, _ in corners)
        centre = (low + high) / 2 if low < 0 < high else max(low, 0.0)
        top = [c for h, c in corners if h >= high - (high - centre) / 4] or [c for _, c in corners]
        horizon = [c for h, c in corners if centre <= h <= centre + (high - centre) / 4] or [c for _, c in corners]

        def mean(cs):
            return tuple(int(sum(c[k] for c in cs) / len(cs)) for k in range(3))
        return SkyLook(zenith=mean(top), horizon=mean(horizon))

    def globals_point(self):
        """The level's Globals point in the game's layout (findScenePoint(Globals, name 0, index 0)), or None."""
        if self.globals is None:
            return None
        g = self.globals
        self.report.say("Globals: clear colour %s, fog %s alpha %d from %.0f to %.0f, far clip %.0f" %
                        (g.clear, g.fog, g.fog_alpha, g.fog_start, g.fog_end, g.far_clip))
        return self.RECORDS.SceneData.POINT(W.GLOBALS, 0, (0.0, 0.0, 0.0), tags=self.SKY.tags(g))

    # -- the model read into level terms --

    def surface_of(self, material_index):
        """The Surface for a glTF material (made on first use)."""
        if material_index is None:
            material_index = -1
        for i, s in enumerate(self.surfaces):
            if getattr(s, "_source", None) == material_index:
                return i
        if material_index < 0:
            name, tags, base, image = "default", {}, (1.0, 1.0, 1.0, 1.0), None
        else:
            m = self.model.materials[material_index]
            name, tags = split_tags(m.name, m.extras)
            base, image = m.base_colour, m.texture
        if image is not None:
            img = self.model.images[image]
            tname = short(split_tags(img.name)[0], self.names) if img.name not in self.textures else img.name
            if tname not in self.textures:
                self.textures[tname] = Texture(tname, img.width, img.height, [list(r) for r in img.rows])
        else:
            tname = short(name + "_colour", self.names)
            px = tuple(int(round(min(1.0, max(0.0, c)) * 255)) for c in base)
            self.textures[tname] = Texture(tname, 8, 8, [[px] * 8 for _ in range(8)])
        full = self.model.materials[material_index].name if material_index >= 0 else name
        s = Surface(full[:NAME_BYTES], tname, two_sided="2sided" in tags and tags["2sided"] != "collide",
                    faces_two_sided="2sided" in tags, base_colour=tuple(base), layer=layer_of(tags))
        for word, kind in SURFACE_KINDS.items():
            if word in tags:
                s.kind = kind
                if word == "slope":
                    try:
                        s.gravity = float(tags[word])
                    except (TypeError, ValueError):
                        s.gravity = 1.0
        s._source = material_index
        self.surfaces.append(s)
        return len(self.surfaces) - 1

    def read_model(self):
        model = self.model
        for node in model.nodes:
            name, tags = split_tags(node.name, node.extras)
            mesh = model.meshes[node.mesh] if node.mesh is not None else None
            mesh_tags = split_tags(mesh.name, mesh.extras)[1] if mesh else {}
            tags = {**mesh_tags, **tags}
            if "zone" in tags:
                self.add_zone(name, node, mesh, tags)
                continue
            if "spawn" in tags:
                spawn = tags["spawn"] if isinstance(tags["spawn"], str) else name
                game_name = self.SPAWN_NAMES.get(spawn.lower(), spawn)
                if game_name is None:
                    self.report.did("spawn %s has no meaning in this game: left out" % spawn)
                else:
                    self.spawns.append((game_name, to_ai(node.translation), quat_to_ai(node.rotation)))
                continue
            if "camera" in tags:
                fov = tags.get("fov")
                try:
                    fov = int(float(fov)) if isinstance(fov, str) else CAMERA_FOV
                except (ValueError, OverflowError):
                    fov = CAMERA_FOV
                self.cameras.append(Camera(to_ai(node.translation), quat_to_ai(node.rotation), max(1, min(179, fov))))
                continue
            if mesh is None:
                continue
            lines = [(p, seg) for p in mesh.primitives for seg in p.lines]
            if lines and "grind" in tags:
                self.add_edges(node, mesh, tags)
            if any(p.triangles for p in mesh.primitives):
                self.add_object(name, node, mesh, tags)

    def add_zone(self, name, node, mesh, tags):
        if mesh is not None and any(p.positions for p in mesh.primitives):
            pts = [to_ai(G.quat_rotate(node.rotation, tuple(c * s for c, s in zip(p, node.scale)))) for prim in mesh.primitives for p in prim.positions]
            pts = [tuple(a + b for a, b in zip(p, to_ai(node.translation))) for p in pts]
        else:
            c = to_ai(node.translation)
            s = [abs(v) for v in node.scale]
            pts = [(c[0] + sx * s[0], c[1] + sy * s[1], c[2] + sz * s[2]) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
        low = tuple(min(p[a] for p in pts) for a in range(3))
        high = tuple(max(p[a] for p in pts) for a in range(3))
        sees = tags.get("sees")
        sees = [s.strip() for s in sees.split(",") if s.strip()] if isinstance(sees, str) else []
        self.zones.append(Zone(name, low, high, sees))

    def add_edges(self, node, mesh, tags):
        word = tags["grind"] if isinstance(tags["grind"], str) else "ledge"
        if word not in GRIND_KINDS:
            raise ValueError("node %r: #grind=%s is not one of %s" % (node.name, word, ", ".join(GRIND_KINDS)))
        kind = GRIND_KINDS[word]
        sound = next((v for k, v in GRIND_SOUNDS.items() if k in tags), 0)
        flags = 0 if kind == 3 else W.EdgeTag.GRINDABLE   # a vertical pole is taken by its own search, not a rail's (TUT)
        for prim in mesh.primitives:
            world = [tuple(a + b for a, b in zip(to_ai(G.quat_rotate(node.rotation, tuple(c * s for c, s in zip(p, node.scale)))), to_ai(node.translation)))
                     for p in prim.positions]
            for i, k in prim.lines:
                self.edge_marks.append(EdgeMark(world[i], world[k], kind, flags, sound))

    def add_object(self, name, node, mesh, tags):
        sky = "sky" in tags
        dynamics = "dynamics" in tags and not sky
        collides = "deco" not in tags and not sky
        scale = tuple(round(s, 6) for s in node.scale)
        key = (node.mesh, scale, collides, sky, dynamics)
        if key not in self.pieces:
            object_name = split_tags(mesh.name)[0] or name
            pieces = self.make_pieces(object_name, mesh, scale, collides, sky)
            name_hash = mesh.extras.get("name_hash") if isinstance(mesh.extras, dict) else None
            if name_hash is not None and len(pieces) == 1:
                pieces[0].name_hash = int(str(name_hash), 0)
            if dynamics:
                if len(pieces) > 1:
                    raise ValueError("dynamics prop %s cut into %d pieces: one prop is one object (at most %d faces, %d vertices)"
                                     % (object_name, len(pieces), FACES_PER_OBJECT, VERTICES_PER_PART))
                pieces[0].dynamics = DynamicsDef.from_extras(mesh.extras.get("dynamics") if isinstance(mesh.extras, dict) else None,
                                                             "node %s" % node.name)
            self.pieces[key] = pieces
        zone = tags.get("zone") if isinstance(tags.get("zone"), str) else None
        self.placements.append(Placement(name, key, to_ai(node.translation), quat_to_ai(node.rotation), collides, zone,
                                         hidden="hidden" in tags, sky=sky, dynamics=dynamics))

    def make_pieces(self, name, mesh, scale, collides, sky=False):
        positions, normals, uvs, colours, faces = [], [], [], [], []
        shared = {}    # (position, normal, uv, colour) -> vertex: equal vertices welded, so strips run on
        for prim in mesh.primitives:
            if not prim.triangles:
                continue
            surface = self.surface_of(prim.material)
            index = []
            for i, p in enumerate(prim.positions):
                position = to_ai((p[0] * scale[0], p[1] * scale[1], p[2] * scale[2]))
                normal = None
                if prim.normals:
                    n = prim.normals[i]
                    n = (n[0] / (scale[0] or 1), n[1] / (scale[1] or 1), n[2] / (scale[2] or 1))
                    length = math.sqrt(sum(c * c for c in n)) or 1.0
                    normal = to_ai(tuple(c / length for c in n))
                uv = tuple(prim.uvs[i][:2]) if prim.uvs else (0.0, 0.0)   # the game reads a BMP top row first: glTF's v as it is
                c = prim.colours[i] if prim.colours else (1.0, 1.0, 1.0, 1.0)
                # 0x80 is 1.0 on the PS2 (the texture as it is); the stock levels go up to 0xFF (about 2.0: the
                # Civic Center's vertex colours are above 0x80 on half of their vertices, 18% at 240 or more). The
                # alpha's full is 0xFF (every stock vertex of the three games has 0xFF): at 0x80 a blended material
                # drew at half its alpha (a converted map's arches faint and see-through, 2026-11-17)
                colour = tuple(min(0xFF, max(0, int(round(v * 0x80)))) for v in c[:3]) + (min(0xFF, max(0, int(round(c[3] * 0xFF)))),)
                key = (position, normal, uv, colour)
                if key not in shared:
                    shared[key] = len(positions)
                    positions.append(position)
                    normals.append(normal)
                    uvs.append(uv)
                    colours.append(colour)
                index.append(shared[key])
            for a, b, c in prim.triangles:
                if len({index[a], index[b], index[c]}) == 3:
                    faces.append((surface, index[a], index[b], index[c]))
        if any(n is None for n in normals):
            face_normals = defaultdict(list)
            for _, a, b, c in faces:
                fn = face_normal(positions[a], positions[b], positions[c])
                for v in (a, b, c):
                    face_normals[v].append(fn)
            for i, n in enumerate(normals):
                if n is None:
                    acc = [sum(fn[k] for fn in face_normals[i]) for k in range(3)] if face_normals[i] else [0.0, -1.0, 0.0]
                    length = math.sqrt(sum(c * c for c in acc)) or 1.0
                    normals[i] = tuple(c / length for c in acc)
        whole = Piece(name, positions, normals, uvs, colours, faces, collides, sky)
        pieces = self.cut(whole)
        if sky and len(pieces) > 1:
            # The world keeps one sky node (setSkyNode: the last one added), so a sky cut in pieces would lose all but one.
            raise ValueError("sky %s has %d faces, %d vertices: one object holds at most %d faces and %d vertices"
                             % (name, len(faces), len(positions), FACES_PER_OBJECT, VERTICES_PER_PART))
        if len(pieces) > 1:
            self.report.did("object %s cut into %d pieces: %d faces over %d an object" % (name, len(pieces), len(faces), FACES_PER_OBJECT))
        used = set()
        for i, p in enumerate(pieces):
            p.name = distinct(name if i == 0 else "%s_%d" % (name, i + 1), used)
        return pieces

    def cut(self, piece):
        """The piece, or its halves at the median of its faces' centres on the longest axis, until each is within
        FACES_PER_OBJECT faces and VERTICES_PER_PART vertices; each half with only the vertices it uses."""
        if len(piece.faces) <= FACES_PER_OBJECT and len(piece.positions) <= VERTICES_PER_PART:
            return [piece]
        centres = [tuple((piece.positions[a][k] + piece.positions[b][k] + piece.positions[c][k]) / 3 for k in range(3)) for _, a, b, c in piece.faces]
        axis = max(range(3), key=lambda k: max(c[k] for c in centres) - min(c[k] for c in centres))
        order = sorted(range(len(piece.faces)), key=lambda i: centres[i][axis])
        halves = [order[:len(order) // 2], order[len(order) // 2:]]
        out = []
        for half in halves:
            remap, positions, normals, uvs, colours, faces = {}, [], [], [], [], []
            for i in half:
                s, a, b, c = piece.faces[i]
                idx = []
                for v in (a, b, c):
                    if v not in remap:
                        remap[v] = len(positions)
                        positions.append(piece.positions[v])
                        normals.append(piece.normals[v])
                        uvs.append(piece.uvs[v])
                        colours.append(piece.colours[v])
                    idx.append(remap[v])
                faces.append((s, *idx))
            out += self.cut(Piece(piece.name, positions, normals, uvs, colours, faces, piece.collides, piece.sky))
        return out

    # -- the level files --

    def build(self):
        self.read_model()
        if not self.placements:
            raise ValueError("the model has no triangle mesh under a node")
        report = self.report
        if self.globals is None:
            # A sky of the model's own (no --sky picture): the level's clear colour and fog from what it shows.
            look = self.sky_look_of([p for key, ps in self.pieces.items() if any(pl.sky and pl.piece_key == key for pl in self.placements) for p in ps])
            if look is not None:
                self.use_sky_look(look)
                report.say("the model's sky: zenith %s, horizon %s" % (look.zenith, look.horizon))
        fit_textures(list(self.textures.values()), report, side=self.TEXTURE_SIDE, budget_kb=self.TEXEL_BUDGET_KB)
        R = self.RECORDS
        objects, by_key, next_id = [], {}, self.FIRST_OBJECT_ID
        definitions = {}    # name hash -> ZDYN
        for key, pieces in self.pieces.items():
            by_key[key] = []
            for piece in pieces:
                obj = self.geo_object(piece, next_id)
                next_id += 2
                if piece.dynamics is not None:
                    obj.type_data = struct.pack("<I", DYNAMICS_OBJECT) + self.dynamics_class_data(piece.dynamics)
                    definitions.setdefault(obj.header.name_hash, piece.dynamics.zdyn(obj.header.name_hash))
                objects.append(obj)
                by_key[key].append((piece, obj))
        # The definitions after every object, as the stock GEOs have them (FORM { TGEO ..., ZDYN ... }).
        geo = R.Geometry(objects, [(len(objects) + n, zdyn) for n, zdyn in enumerate(definitions.values())])
        if definitions:
            report.say("%d dynamics props: %s" % (len(definitions), ", ".join(
                "%s (%08X)" % (o.header.name, o.header.name_hash) for o in objects if o.header.name_hash in definitions and o.type_data[:4] == struct.pack("<I", DYNAMICS_OBJECT))))
        instances, skies = [], []
        for pl in self.placements:
            for piece, obj in by_key[pl.piece_key]:
                name = short(pl.node_name if piece is by_key[pl.piece_key][0][0] else "%s_%d" % (pl.node_name, by_key[pl.piece_key].index((piece, obj)) + 1), self.names)
                inst = R.Instance.make(name, obj, pl.position, pl.rotation, collides=pl.collides, tags=struct.pack("<I", TERRAIN))
                inst.flags = INSTANCE_FLAGS_COLLIDES if pl.collides else INSTANCE_FLAGS_DECORATION
                if pl.hidden:
                    inst.flags &= ~INSTANCE_FLAG_SHOWN
                if pl.sky:
                    inst.flags = self.SKY.instance_flags
                    skies.append((inst, obj))
                if pl.dynamics:
                    inst.flags = self.DYNAMICS.instance_flags
                    inst.tags = self.dynamics_instance_tags(piece.dynamics)
                if pl.zone:
                    inst.region = L.name_hash(pl.zone)
                instances.append(inst)
        # A sky sits at the level's centre: the game moves it to the camera each frame (drawSky), so its place
        # only sets its box, and a box away from the level (a dome at the origin of a level built off it)
        # stretches the world box and its grid, and the level's objects stop being drawn.
        others = [i for i in instances if not i.flags & INSTANCE_FLAG_SKY]
        if skies and others:
            low, high = self.world_box(others)
            centre = tuple((low[k] + high[k]) / 2 for k in range(3))
            for inst, obj in skies:
                moved = R.Instance.make(inst.name, obj, centre, inst.rotation, collides=False, tags=inst.tags)
                moved.flags = inst.flags
                instances[instances.index(inst)] = moved
            self.report.say("sky %s placed at the level's centre (%.0f, %.0f, %.0f)" % (", ".join(i.name for i, _ in skies), *centre))
        self.tag_edges(geo, instances)
        ins = R.Instances(instances)
        dat = self.scene_data()
        low, high = self.world_box(instances)
        files = {"GEO": geo.write(), "INS": ins.write(), "DAT": dat.write(), "TXR": self.texture_file(), "ZIT": self.scene_file(low, high)}
        self.budgets(geo, instances, dat)
        return files

    def dynamics_class_data(self, definition):
        """The TGTD bytes after DynamicsObject's word: the definition's own when it is this game's, else DYNAMICS's."""
        return definition.class_data if definition.game == self.DYNAMICS.game else self.DYNAMICS.class_data

    def dynamics_instance_tags(self, definition):
        return definition.instance_tags if definition.game == self.DYNAMICS.game and definition.instance_tags else self.DYNAMICS.instance_tags

    def texture_file(self):
        return write_txr(list(self.textures.values()))

    def check_level_bytes(self, files):
        """The level's files within the game's memory (LEVEL_BYTES): over it, an error in the report."""
        total = sum(len(files[ext]) for ext in ("GEO", "INS", "DAT", "TXR"))
        self.report.say("level files: %d bytes (GEO, INS, DAT, TXR; budget %d, %s)" % (total, self.LEVEL_BYTES, self.LEVEL_BYTES_SOURCE))
        if total > self.LEVEL_BYTES:
            self.report.error("%d bytes of level files over %d (%s): the level does not fit the game's memory"
                              % (total, self.LEVEL_BYTES, self.LEVEL_BYTES_SOURCE))

    def scene_file(self, low, high):
        return zit(self.slot, low, high)

    def geo_object(self, piece, object_id):
        part_id = object_id + 1
        surfaces = sorted({s for s, _, _, _ in piece.faces})
        local = {s: i for i, s in enumerate(surfaces)}
        faces = []
        for s, a, b, c in piece.faces:
            faces.append(W.Face.make(local[s], a, b, c, collides=piece.collides, two_sided=self.surfaces[s].faces_two_sided or self.surfaces[s].two_sided))
        groups = []
        for s in surfaces:
            tris = [(a, b, c) for ss, a, b, c in piece.faces if ss == s]
            groups += pack_strips(local[s], stripify(tris))
        if len(piece.positions) > VERTICES_PER_PART:
            raise ValueError("%s: %d vertices in one part" % (piece.name, len(piece.positions)))
        face_normals = [struct.pack("<3f", *face_normal(piece.positions[a], piece.positions[b], piece.positions[c])) for _, a, b, c in piece.faces]
        part = W.Part(part_id, [tuple(p) for p in piece.positions], faces, groups,
                      normals=W.IndexedStream.make([struct.pack("<3f", *n) for n in piece.normals]),
                      colours=W.IndexedStream.make([struct.pack("<4B", *c) for c in piece.colours]),
                      uvs=W.IndexedStream.make([struct.pack("<2f", *uv) for uv in piece.uvs]),
                      face_normals=W.IndexedStream.make(face_normals),
                      flags=self.SKY.part_flags if piece.sky else PART_FLAGS, material_count=len(surfaces))
        tags = []
        for i, (s, a, b, c) in enumerate(piece.faces):
            tags.append(self.surfaces[s].kind)
        kinds = sorted({k for k in tags if k})
        if kinds and piece.collides:
            records = []
            for k in kinds:
                gravity = max((self.surfaces[s].gravity for s, _, _, _ in piece.faces if self.surfaces[s].kind == k), default=0.0)
                records.append(W.FaceTag(0, k, gravity=gravity if k == 3 else 0.0, launch_push=QP_LAUNCH_PUSH if k == 1 else 0.0))
            part.face_tags = W.TagBuffer(records, [kinds.index(k) if k else None for k in tags])
        table = Chunk(b"TMTB", kids=[mati(self.surfaces[s].name, self.surfaces[s].texture, self.surfaces[s].two_sided, self.surfaces[s].layer) for s in surfaces])
        lo = tuple(min(p[k] for p in piece.positions) for k in range(3))
        hi = tuple(max(p[k] for p in piece.positions) for k in range(3))
        header = W.GeoHeader(lo, hi, object_id, piece.name, piece.name_hash)
        obj = W.GeoObject(header, b"\x00\x00\x01\x00", [(part, W.LevelOfDetail(LOD_DISTANCE, part_id), table)], type_data=struct.pack("<I", TERRAIN))
        if piece.collides:
            obj.collision = W.CollisionTree.build(part_id, part.positions, faces)
        return obj

    def tag_edges(self, geo, instances):
        """Every collidable face edge lying along a grind mark gets that mark's record, on the object (shared
        by its instances; the first placement decides)."""
        if not self.edge_marks:
            return
        marks = self.edge_marks
        index = MarkIndex(marks)
        objects = {o.header.id: o for o in geo.objects}
        tagged = set()
        for inst in instances:
            if not (inst.flags & W.Instance.COLLIDES) or inst.geometry_id in tagged:
                continue
            obj = objects[inst.geometry_id]
            part = obj.parts[0][0]
            world = [tuple(a + b for a, b in zip(L.quat_rotate(inst.rotation, p), inst.position)) for p in part.positions]
            # One record per tagged face edge, as the stock levels have it (HANGER: every TGFE's record count is
            # its tagged edge count): the game's rail search walks an object's records and takes each record's
            # own edge (railFilter, considerEdge: edgeId(record), the element the load writes from the last face
            # edge that picks the record), so a record shared by many edges leaves one rail on the object.
            records, indices = [], [None] * (3 * len(part.faces))
            kinds = Counter()
            for f, face in enumerate(part.faces):
                for k in range(3):
                    a, b = world[face.vertices[k]], world[face.vertices[(k + 1) % 3]]
                    mark = next((m for m in index.near(a) if on_segment(a, m) and on_segment(b, m)), None)
                    if mark is None:
                        continue
                    indices[3 * f + k] = len(records)
                    records.append(W.EdgeTag(0, mark.kind, mark.flags, mark.sound))
                    kinds[mark.kind] += 1
            if records:
                part.edge_tags = W.TagBuffer(records, indices)
                tagged.add(inst.geometry_id)
                self.report.say("%s: %d grind face edges, %s" % (obj.header.name, len(records),
                                                                 ", ".join("kind %d x%d" % kv for kv in sorted(kinds.items()))))

    def start_camera(self):
        """An idle camera for a level with none of its own: START_CAMERA_BACK behind the PlayerOne spawn (along its
        facing across the ground) and START_CAMERA_UP above it, looking at a point START_CAMERA_LOOK_UP above the
        spawn. None without a PlayerOne."""
        start = next(((p, r) for n, p, r in self.spawns if L.name_hash(n) == W.PLAYER_ONE), None)
        if start is None:
            return None
        p, (x, y, z, w) = start
        fx, fz = 2.0 * (x * z + w * y), 1.0 - 2.0 * (x * x + y * y)   # the spawn's +z, turned
        n = math.sqrt(fx * fx + fz * fz)
        fx, fz = (fx / n, fz / n) if n > 1e-6 else (0.0, 1.0)
        eye = (p[0] - START_CAMERA_BACK * fx, p[1] - START_CAMERA_UP, p[2] - START_CAMERA_BACK * fz)   # (y grows down)
        d = (p[0] - eye[0], p[1] - START_CAMERA_LOOK_UP - eye[1], p[2] - eye[2])
        length = math.sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])
        return Camera(eye, look_rotation((d[0] / length, d[1] / length, d[2] / length)))

    def camera_points(self):
        """The idle cameras as the game's "CameraPos" points named "idle", numbered from 1 (findScenePoint's index;
        BMX XXX's idle mode takes 1 to N in turn), each its tag record's first byte the field of view. Made at the
        start when the model has none and the game reads them (MAKES_CAMERA)."""
        cameras = self.cameras
        if not cameras and self.MAKES_CAMERA:
            made = self.start_camera()
            if made is not None:
                cameras = [made]
                self.report.did("no camera in the model: an idle camera made behind and above the PlayerOne spawn, at (%.2f, %.2f, %.2f)"
                                % made.position)
        points = []
        for i, c in enumerate(cameras):
            position = (c.position[0], c.position[1] - self.CAMERA_LIFT, c.position[2])
            points.append(self.RECORDS.SceneData.POINT(CAMERA_POINT, IDLE_CAMERA, position, c.rotation, i + 1, bytes([c.fov, 0, 0, 0])))
        if points:
            self.report.say("idle cameras: %d" % len(points))
        return points

    def scene_data(self):
        points = []
        for name, position, rotation in self.spawns:
            points.append(W.ScenePoint.spawn(name, position, rotation, tags=self.SPAWN_TAGS))
        if not any(p.name == W.PLAYER_ONE for p in points):
            self.report.error("no PlayerOne spawn (an empty node tagged #spawn=PlayerOne): the skater has nowhere to start")
        points += self.camera_points()
        if self.globals is not None:
            points.append(self.globals_point())
        regions = None
        if self.zones:
            names = [z.name for z in self.zones]
            regions = []
            for z in self.zones:
                for s in z.sees:
                    if s not in names:
                        self.report.error("zone %s sees %r, which is no zone" % (z.name, s))
                hidden = [L.name_hash(n) for n in names if n != z.name and n not in z.sees]
                regions.append(W.ViewRegion(L.name_hash(z.name), z.low, z.high, hidden, 0))
        return W.SceneData(points, regions)

    def world_box(self, instances):
        low = [min(i.world_min[k] for i in instances) for k in range(3)]
        high = [max(i.world_max[k] for i in instances) for k in range(3)]
        low = tuple(math.floor(v / CELL) * CELL - CELL for v in low)
        high = tuple(math.ceil(v / CELL) * CELL + CELL for v in high)
        return low, high

    def budgets(self, geo, instances, dat):
        r = self.report
        parts = {o.header.id: len(o.parts) for o in geo.objects}
        nv = max(len(p.positions) for o in geo.objects for p, _, _ in o.parts)
        nf = max(len(p.faces) for o in geo.objects for p, _, _ in o.parts)
        ng = max(len(p.groups) for o in geo.objects for p, _, _ in o.parts)
        biggest_group = max(len(g.entries) for o in geo.objects for p, _, _ in o.parts for g in p.groups)
        tree = max((len(o.collision.faces()) for o in geo.objects if o.collision), default=0)
        texels = sum(t.texels for t in self.textures.values())
        collidable = sum(len(p.faces) for o in geo.objects if o.collision for p, _, _ in o.parts)
        r.say("%s (%s): %d objects, %d parts, %d instances, %d faces (%d collidable in %d trees), %d spawns, %d zones" %
              (self.level_name, self.slot, len(geo.objects), sum(parts.values()), len(instances), sum(len(p.faces) for o in geo.objects for p, _, _ in o.parts),
               collidable, sum(1 for o in geo.objects if o.collision), len(self.spawns), len(self.zones)))
        r.say("textures: %d, %d KB of texels (budget %d KB), largest %dx%d (side budget %d): %s" %
              (len(self.textures), texels // 1024, self.TEXEL_BUDGET_KB, max(t.width for t in self.textures.values()), max(t.height for t in self.textures.values()),
               self.TEXTURE_SIDE, ", ".join("%s %dx%d" % (t.name, t.width, t.height) for t in self.textures.values())))
        r.say("largest part: %d vertices (budget %d), %d faces (budget %d), %d strip groups of at most %d entries (budget %d); largest tree %d faces" %
              (nv, VERTICES_PER_PART, nf, FACES_PER_OBJECT, ng, biggest_group, W.StripGroup.GROUP_ENTRIES, tree))
        if dat.regions:
            in_zone, nowhere = Counter(), 0
            for i in instances:
                z = i.region or next((z.mesh for z in dat.regions if all(z.low[k] <= i.position[k] <= z.high[k] for k in range(3))), 0)
                if z:
                    in_zone[z] += parts[i.geometry_id]
                else:
                    nowhere += parts[i.geometry_id]
            views = {}
            for z in dat.regions:
                views[z.mesh] = sum(n for m, n in in_zone.items() if m not in z.hidden) + nowhere
            worst = max(views.values())
            r.say("parts in view: at most %d from one zone (budget %d, %s); %d parts in no zone" % (worst, self.PARTS_IN_VIEW, self.PARTS_IN_VIEW_SOURCE, nowhere))
            if worst > self.PARTS_IN_VIEW:
                r.error("%d parts drawn from one zone, over %d: the model needs more zones (#zone) hiding each other" % (worst, self.PARTS_IN_VIEW))
        else:
            total = sum(parts.values())
            r.say("parts in view: %d (no zones; budget %d, %s)" % (total, self.PARTS_IN_VIEW, self.PARTS_IN_VIEW_SOURCE))
            if total > self.PARTS_IN_VIEW:
                r.error("%d parts and no visibility zones: over %d in every view" % (total, self.PARTS_IN_VIEW))
        if texels > self.TEXEL_BUDGET_KB * 1024:
            r.error("%d KB of texels over the %d KB budget" % (texels // 1024, self.TEXEL_BUDGET_KB))
        for name, kind in sorted({(s.name, s.kind) for s in self.surfaces if s.kind}):
            r.say("surface %s: kind %d" % (name, kind))


def face_normal(a, b, c):
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    length = math.sqrt(sum(k * k for k in n)) or 1.0
    return (n[0] / length, n[1] / length, n[2] / length)


EDGE_TOLERANCE = 0.01
MARK_CELL = 4.0


class MarkIndex:
    """The grind marks by the MARK_CELL cells their segments cross (plus EDGE_TOLERANCE), so a face edge
    looks only at the marks near its first point, in the marks' own order: the same first match as a scan
    of every mark, in a fraction of the time for a level with thousands of marks."""

    def __init__(self, marks):
        self.cells = defaultdict(list)
        for n, m in enumerate(marks):
            low = [min(m.a[k], m.b[k]) - EDGE_TOLERANCE for k in range(3)]
            high = [max(m.a[k], m.b[k]) + EDGE_TOLERANCE for k in range(3)]
            for x in range(int(low[0] // MARK_CELL), int(high[0] // MARK_CELL) + 1):
                for y in range(int(low[1] // MARK_CELL), int(high[1] // MARK_CELL) + 1):
                    for z in range(int(low[2] // MARK_CELL), int(high[2] // MARK_CELL) + 1):
                        self.cells[(x, y, z)].append((n, m))

    def near(self, p):
        """The marks whose cells hold p, in their original order."""
        return [m for _, m in sorted(self.cells.get((int(p[0] // MARK_CELL), int(p[1] // MARK_CELL), int(p[2] // MARK_CELL)), ()))]


def on_segment(p, mark):
    """p within EDGE_TOLERANCE of the segment a-b."""
    a, b = mark.a, mark.b
    ab = tuple(b[k] - a[k] for k in range(3))
    length2 = sum(c * c for c in ab)
    if length2 < 1e-12:
        return False
    t = sum((p[k] - a[k]) * ab[k] for k in range(3)) / length2
    if t < -EDGE_TOLERANCE or t > 1 + EDGE_TOLERANCE:
        return False
    t = max(0.0, min(1.0, t))
    q = tuple(a[k] + t * ab[k] for k in range(3))
    return math.dist(p, q) <= EDGE_TOLERANCE


# ---- the other files of a level folder ------------------------------------------------------

def zit(slot, low, high, cell=CELL):
    """FSCN { TERR { TEHD, ZFTX, ZFGE, ZFIS, ZFSD } }: the collision world's cell and box (the counts after
    them as the exporter wrote them; the game reads the first 0x24 bytes) and the level's file names
    (zge_scene.SceneFile, the same form in all three games)."""
    return Z.SceneFile.make(slot.lower(), low, high, cell).write()


def empty_script():
    """The smallest level script the game runs (version 2.70, as the Zoo prototype's 60-byte ZOOT.BIN and
    TUTT.BIN): one object with one empty state, one variable word."""
    state, obj = 0x04588EA5, 0x0049A229
    words = [state, obj, 3, 0xFFFFFFFF, 0, 0, state, 5, 0xFFFFFFFF, 0xFFFFFFFF]
    return struct.pack("<f4I", 2.70, 9, 1, 0, 0) + struct.pack("<%dI" % len(words), *words)


def empty_library():
    """A ZDIR asset library with no entries (zge/assets/asset_library.h): MOTIONS.ZAL for a level with no
    animated objects."""
    return L.serialize([Chunk(b"ZDIR", kids=[Chunk(b"DIDF", struct.pack("<2I", 0, 0)), Chunk(b"GRID", b""), Chunk(b"DLEN", b""), Chunk(b"OFST", b"")])])


def convert(model_path, out_dir=None, slot="TUT", level_name=None, force=False, sky=None, sky_radius=SKY_RADIUS):
    conv = Converter(G.read_model(model_path), slot, level_name, force)
    if sky:
        conv.use_sky(sky, sky_radius)
    files = conv.build()
    conv.check_level_bytes(files)
    report = conv.report
    if report.errors and not force and out_dir:
        print(report.text())
        raise SystemExit("ai_level_convert.py: over budget (see above; --force writes anyway)")
    if out_dir:
        folder = os.path.join(out_dir, "ASSETS", "LEVELS", conv.slot)
        os.makedirs(folder, exist_ok=True)
        written = {}
        for ext, data in files.items():
            written["%s.%s" % (conv.slot, ext)] = data
        written["%s.BIN" % conv.slot] = empty_script()
        written["%sT.BIN" % conv.slot] = empty_script()
        written["MOTIONS.ZAL"] = empty_library()
        for name, data in written.items():
            with open(os.path.join(folder, name), "wb") as f:
                f.write(data)
        report.say("written to %s: %s" % (folder, ", ".join("%s %d B" % (n, len(d)) for n, d in written.items())))
    print(report.text())
    return conv, files


# ---- the example park ---------------------------------------------------------------------------

def lcg(seed):
    """A small deterministic noise source."""
    state = seed & 0xFFFFFFFF
    while True:
        state = (1103515245 * state + 12345) & 0xFFFFFFFF
        yield (state >> 16) & 0x7FFF


def concrete_texture(side):
    """Grey concrete with seams every quarter and a dark F in the top-left quarter (asymmetric both ways, so a
    picture tells the texture's orientation)."""
    noise = lcg(7)
    rows = []
    q = side // 4
    for y in range(side):
        row = []
        for x in range(side):
            v = 150 + (next(noise) % 25)
            if x % (side // 2) in (0, 1) or y % (side // 2) in (0, 1):
                v -= 50
            fx, fy = x / q, y / q   # the F in the first quarter: a stem and two bars
            if 0.2 <= fx <= 0.35 and 0.15 <= fy <= 0.85 or 0.2 <= fx <= 0.8 and 0.15 <= fy <= 0.28 or 0.2 <= fx <= 0.65 and 0.45 <= fy <= 0.56:
                v = 40
            row.append((v, v, v - 6, 255))
        rows.append(row)
    return rows


def wood_texture(side):
    noise = lcg(11)
    rows = []
    for y in range(side):
        plank = (y * 6 // side) % 2
        row = []
        for x in range(side):
            g = next(noise) % 30
            base = (150, 100, 55) if plank else (165, 112, 62)
            seam = (y * 6 % side) < 2
            row.append(tuple(max(0, c + g - 15 - (60 if seam else 0)) for c in base) + (255,))
        rows.append(row)
    return rows


def metal_texture(side):
    rows = []
    for y in range(side):
        row = []
        for x in range(side):
            v = 120 + (x * 60 // side)
            row.append((v, v + 4, v + 10, 255))
        rows.append(row)
    return rows


def sky_texture(width, height):
    """A blue sky for --sky (add_sky's v: the zenith at the top row, the horizon three quarters down, 30 degrees
    below it at the bottom): deep blue at the zenith to a pale haze at the horizon, with soft clouds (clusters of
    round puffs, wider than tall) in the band above it, wrapping across so the dome's four repeats meet."""
    zenith, horizon, cloud = (58, 112, 196), (178, 206, 234), (244, 246, 250)
    noise = lcg(23)
    puffs = []
    for _ in range(9):
        cx, cv = (next(noise) % 1000) / 1000.0, 0.4 + (next(noise) % 220) / 1000.0
        for _ in range(4):
            puffs.append((cx + ((next(noise) % 120) - 60) / 1000.0, cv + ((next(noise) % 40) - 20) / 1000.0,
                          0.012 + (next(noise) % 22) / 1000.0))
    rows = []
    for y in range(height):
        v = y / (height - 1)
        t = min(1.0, v / 0.75) ** 1.6
        row = []
        for x in range(width):
            u = x / width
            c = [zenith[k] + (horizon[k] - zenith[k]) * t for k in range(3)]
            cover = 0.0
            for pu, pv, r in puffs:
                du = min(abs(u - pu) % 1.0, 1.0 - abs(u - pu) % 1.0) / 2.5
                cover += math.exp(-((du * du + (v - pv) ** 2) / (r * r)))
            cover = (1.0 - math.exp(-1.5 * cover)) * 0.8
            row.append(tuple(int(round(c[k] + (cloud[k] - c[k]) * cover)) for k in range(3)) + (255,))
        rows.append(row)
    return rows


class MeshBuilder:
    """Triangles with planar UVs and face normals, for the park's pieces (glTF axes, +y up)."""

    def __init__(self):
        self.positions, self.normals, self.uvs, self.triangles = [], [], [], []

    def quad(self, a, b, c, d, uv_scale=0.25, normal=None):
        """a b c d counter-clockwise seen from the front (the side the normal faces)."""
        n = normal or face_normal_up(a, b, c)
        axis = max(range(3), key=lambda k: abs(n[k]))
        u_axis, v_axis = [k for k in range(3) if k != axis]
        base = len(self.positions)
        for p in (a, b, c, d):
            self.positions.append(tuple(p))
            self.normals.append(n)
            self.uvs.append((p[u_axis] * uv_scale, p[v_axis] * uv_scale))
        self.triangles += [(base, base + 1, base + 2), (base, base + 2, base + 3)]

    def box(self, lo, hi, uv_scale=0.25, faces=("top", "bottom", "front", "back", "left", "right")):
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        if "top" in faces:
            self.quad((x0, y1, z1), (x1, y1, z1), (x1, y1, z0), (x0, y1, z0), uv_scale)
        if "bottom" in faces:
            self.quad((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), uv_scale)
        if "front" in faces:   # +z
            self.quad((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), uv_scale)
        if "back" in faces:    # -z
            self.quad((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0), uv_scale)
        if "left" in faces:    # -x
            self.quad((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0), uv_scale)
        if "right" in faces:   # +x
            self.quad((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), uv_scale)

    def primitive(self, material):
        return dict(positions=self.positions, normals=self.normals, uvs=self.uvs, triangles=self.triangles, material=material)


def face_normal_up(a, b, c):
    return face_normal(a, b, c)


def make_example(out_dir):
    """The test park: an 80 x 80 m concrete ground in 2 m tiles (3,200 faces: cut into pieces by the
    converter), a wooden quarter pipe 12 m wide with a 3 m transition and a metal coping, a 10 m flat
    bar, a 12 m concrete ledge, the PlayerOne spawn and two visibility zones that see each other. The
    concrete texture is 256 x 256 so the converter has to halve it."""
    b = G.Builder()
    concrete = b.material("concrete", texture=b.image("concrete", concrete_texture(256)))
    wood = b.material("wood", texture=b.image("wood", wood_texture(128)))
    wood_qp = b.material("wood#qp", texture=1)
    metal = b.material("metal", texture=b.image("metal", metal_texture(64)))

    # ground: a grid of 2 m cells, +y up, counter-clockwise from above
    g = MeshBuilder()
    n, size = 40, 80.0
    for i in range(n):
        for k in range(n):
            x0, x1 = -size / 2 + i * size / n, -size / 2 + (i + 1) * size / n
            z0, z1 = -size / 2 + k * size / n, -size / 2 + (k + 1) * size / n
            g.quad((x0, 0.0, z1), (x1, 0.0, z1), (x1, 0.0, z0), (x0, 0.0, z0), 0.25, (0.0, 1.0, 0.0))
    b.node("ground", b.mesh("ground", [g.primitive(concrete)]))

    # quarter pipe at the far end (-z), facing the spawn (+z): the transition's arc, the deck, the back and sides
    R, width, z0, deck = 3.0, 12.0, -16.0, 2.0
    arc = [(z0 - R * math.sin(t), R - R * math.cos(t)) for t in (math.radians(90 * k / 8) for k in range(9))]
    qp, rest = MeshBuilder(), MeshBuilder()
    for k in range(8):
        (za, ya), (zb, yb) = arc[k], arc[k + 1]
        for s in range(6):
            xa, xb = -width / 2 + s * width / 6, -width / 2 + (s + 1) * width / 6
            qp.quad((xa, ya, za), (xb, ya, za), (xb, yb, zb), (xa, yb, zb), 0.25)
    rest.quad((-width / 2, R, z0 - R), (width / 2, R, z0 - R), (width / 2, R, z0 - R - deck), (-width / 2, R, z0 - R - deck), 0.25, (0.0, 1.0, 0.0))
    rest.quad((width / 2, 0.0, z0 - R - deck), (-width / 2, 0.0, z0 - R - deck), (-width / 2, R, z0 - R - deck), (width / 2, R, z0 - R - deck), 0.25)
    outline = [(z, y) for z, y in arc] + [(z0 - R - deck, R), (z0 - R - deck, 0.0)]
    for x, sign in ((-width / 2, -1.0), (width / 2, 1.0)):
        corner = (x, 0.0, z0 - R - deck)
        base = len(rest.positions)
        pts = [corner] + [(x, y, z) for z, y in outline[:-1]]
        for p in pts:
            rest.positions.append(p)
            rest.normals.append((sign, 0.0, 0.0))
            rest.uvs.append((p[2] * 0.25, p[1] * 0.25))
        for k in range(1, len(pts) - 1):
            tri = (base, base + k, base + k + 1) if sign < 0 else (base, base + k + 1, base + k)
            rest.triangles.append(tri)
    b.node("qpipe_01", b.mesh("qpipe_01", [qp.primitive(wood_qp), rest.primitive(wood)]))
    coping = dict(positions=[(-width / 2, R, z0 - R), (width / 2, R, z0 - R)], lines=[(0, 1)])
    b.node("qpipe_01_coping#grind=lip#metal", b.mesh("qpipe_01_coping", [coping]))

    # a flat bar: 10 m along z at x = -8, 0.5 m high, on two posts
    bar = MeshBuilder()
    bar.box((-8.05, 0.42, -5.0), (-7.95, 0.5, 5.0), 1.0)
    bar.box((-8.03, 0.0, -4.4), (-7.97, 0.42, -4.3), 1.0)
    bar.box((-8.03, 0.0, 4.3), (-7.97, 0.42, 4.4), 1.0)
    b.node("flat_bar", b.mesh("flat_bar", [bar.primitive(metal)]))
    rail_edges = dict(positions=[(-8.05, 0.5, -5.0), (-8.05, 0.5, 5.0), (-7.95, 0.5, -5.0), (-7.95, 0.5, 5.0)], lines=[(0, 1), (2, 3)])
    b.node("flat_bar_edges#grind#metal", b.mesh("flat_bar_edges", [rail_edges]))

    # a ledge: 12 m along z at x = 6..7, 0.45 m high
    ledge = MeshBuilder()
    ledge.box((6.0, 0.0, -6.0), (7.0, 0.45, 6.0), 0.5, faces=("top", "front", "back", "left", "right"))
    b.node("ledge", b.mesh("ledge", [ledge.primitive(concrete)]))
    ledge_edges = dict(positions=[(6.0, 0.45, -6.0), (6.0, 0.45, 6.0), (7.0, 0.45, -6.0), (7.0, 0.45, 6.0)], lines=[(0, 1), (2, 3)])
    b.node("ledge_edges#grind", b.mesh("ledge_edges", [ledge_edges]))

    # the skater's start, 1 m above the ground, 14 m before the pipe, facing it (a spawn's identity faces -z here, +z in
    # the game: the skater was placed facing (0, 0, 1) in the game with it)
    b.node("PlayerOne#spawn", translation=(0.0, 1.0, 14.0))
    # two visibility zones, each seeing the other
    b.node("west#zone#sees=east", translation=(-22.5, 10.0, 0.0), scale=(22.5, 20.0, 45.0))
    b.node("east#zone#sees=west", translation=(22.5, 10.0, 0.0), scale=(22.5, 20.0, 45.0))
    path = os.path.join(out_dir, "park.gltf")
    b.write(path)
    # its sky, for --sky (not part of the model)
    with open(os.path.join(out_dir, "sky.png"), "wb") as f:
        f.write(G.write_png(sky_texture(256, 128)))
    return path


# ---- commands --------------------------------------------------------------------------------

def cmd_convert(args):
    convert(args.model, args.out, args.slot, args.level_name, args.force, args.sky, args.sky_radius)


def cmd_info(args):
    convert(args.model, None, args.slot, args.level_name, True)


def cmd_example(args):
    path = make_example(args.out)
    print("written %s" % path)


# A prop for the tests: Woodward's traffic cone's definition (DM2's disc, dynamicsDef_ConesA's ZDYN after its DDEF: one
# box body), under a name longer than the TGHD's field.
EXAMPLE_PROP_NAME = "DynamicsDef_ExampleConeLongerThanItsField"
EXAMPLE_PROP_DEFINITION = {"game": "dm2", "class": "", "instance_tags": "000000000c000000", "zdyn": [[
    "DBDY", "00000000c1857b0a00002041000048baffff51420000000000000000f4940a42ffff514238be2442"]]}


def make_prop_example(out_dir):
    """A 20 x 20 m ground and two cones, #dynamics props of one object named EXAMPLE_PROP_NAME: the test of the props'
    path and of a name its header cuts (zge_level_gltf.py's selftest exports and converts it again)."""
    b = G.Builder()
    concrete = b.material("concrete", texture=b.image("concrete", concrete_texture(64)))
    orange = b.material("cone", (1.0, 0.4, 0.0, 1.0))
    g = MeshBuilder()
    g.quad((-10.0, 0.0, 10.0), (10.0, 0.0, 10.0), (10.0, 0.0, -10.0), (-10.0, 0.0, -10.0), 0.25, (0.0, 1.0, 0.0))
    b.node("ground", b.mesh("ground", [g.primitive(concrete)]))
    cone = MeshBuilder()
    cone.box((-0.2, 0.0, -0.2), (0.2, 0.8, 0.2), 1.0)
    mesh = b.mesh(EXAMPLE_PROP_NAME, [cone.primitive(orange)], extras={"dynamics": EXAMPLE_PROP_DEFINITION})
    b.node("cone_01#dynamics", mesh, translation=(2.0, 0.0, 0.0))
    b.node("cone_02#dynamics", mesh, translation=(-2.0, 0.0, 0.0))
    b.node("PlayerOne#spawn", translation=(0.0, 1.0, 8.0))
    path = os.path.join(out_dir, "props.gltf")
    b.write(path)
    return path


def prop_terms(geo, ins, records):
    """A level's props as the game finds them: (object name, its name hash, the hash its ZDYN's DDEF has, instance
    count) for every object of the DynamicsObject type."""
    definitions = {struct.unpack_from("<I", k.kids[0].data)[0] for _, k in geo.other if k.tag == b"ZDYN" and k.kids and k.kids[0].tag == b"DDEF"}
    placed = Counter(i.geometry_id for i in ins.instances)
    return sorted((o.header.name, o.header.name_hash, o.header.name_hash in definitions, placed[o.header.id]) for o in geo.objects
                  if o.type_data and o.type_data[:4] == struct.pack("<I", DYNAMICS_OBJECT))


def cmd_selftest(args):
    import tempfile
    # strips: every triangle once, with its winding, in groups of at most 60
    tris = [(0, 1, 2), (2, 1, 3), (2, 3, 4), (4, 3, 5), (10, 11, 12)]
    strips = stripify(tris)
    assert sorted(decode_strips(strips)) == sorted(cyclic(t) for t in tris), strips
    groups = [g for s in strips for g in W.StripGroup.from_strip(0, s)]
    assert all(len(g.entries) <= 60 for g in groups) and len(strips) == 2, strips
    # a cut-out halved stays one: one-texel bars solid in their colour, not half clear and darkened; a translucent
    # texture is averaged as before
    bars = Texture("bars", 4, 4, [[(40, 200, 180, 255) if x % 2 == 0 else (0, 0, 0, 0) for x in range(4)] for y in range(4)])
    assert bars.cut_out
    bars.halve()
    assert (bars.width, bars.height) == (2, 2) and all(px == (40, 200, 180, 255) for row in bars.rows for px in row), bars.rows
    glass = Texture("glass", 2, 2, [[(100, 100, 100, 120), (200, 200, 200, 0)], [(100, 100, 100, 120), (200, 200, 200, 0)]])
    assert not glass.cut_out
    glass.halve()
    assert glass.rows == [[(150, 150, 150, 60)]], glass.rows
    # a palette of more than 256 colours is cut to 256
    rows = [[(x, y, (x * y) & 0xFF, 255) for x in range(64)] for y in range(64)]
    palette, index_rows = palettize(rows, False)
    assert len(palette) <= 256 and len(index_rows) == 64 and max(max(r) for r in index_rows) < len(palette)
    bmp = bmp8(64, 64, palette, index_rows, False)
    assert bmp[:2] == b"BM" and len(bmp) % 4 == 0 and struct.unpack_from("<I", bmp, 2)[0] == len(bmp)
    with tempfile.TemporaryDirectory() as tmp:
        model = make_example(os.path.join(tmp, "model"))
        conv, files = convert(model, os.path.join(tmp, "mods"), "TUT", "Test Park")
        folder = os.path.join(tmp, "mods")
        disc = L.Disc(folder)
        level = L.read_level(disc, "TUT")
        assert level["GEO"] == files["GEO"] and level["ZIT"] == files["ZIT"]
        # the writers read their own files back
        geo = W.Geometry.read(level["GEO"])
        W.Instances.read(level["INS"])
        dat = W.SceneData.read(level["DAT"])
        # the ground is at y 0 under the spawn, the ledge top at -0.45, the deck at -3, nothing past the park
        groups = L.collision_triangles(level["GEO"], level["INS"])
        assert abs(L.topmost(groups, 0.0, -14.0) - 0.0) < 1e-4
        assert abs(L.topmost(groups, 6.5, 0.0) + 0.45) < 1e-4, L.topmost(groups, 6.5, 0.0)
        assert abs(L.topmost(groups, 0.0, 20.5) + 3.0) < 1e-4, L.topmost(groups, 0.0, 20.5)
        assert L.topmost(groups, 60.0, 0.0) is None
        # the grind edges: the coping (kind 1) 12 m long, the bar's two edges and the ledge's two (kind 0)
        edges = L.grind_edges(level["GEO"], level["INS"])
        kinds = Counter(k for _, _, _, k in edges)
        assert kinds[1] >= 2 and kinds[0] >= 4, kinds
        coping = [e for e in edges if e[3] == 1]
        assert all(abs(a[1] + 3.0) < 1e-4 and abs(a[2] - 19.0) < 1e-4 for a, b, _, _ in coping), coping[:2]
        bar = [e for e in edges if e[3] == 0 and abs(e[0][0] + 8.0) < 0.1]
        assert bar and all(abs(a[1] + 0.5) < 1e-4 for a, b, _, _ in bar), bar[:2]
        # the ground was cut into pieces of at most 1,024 faces, the 256 texture halved, the budgets reported
        ground = [o for o in geo.objects if o.header.name.startswith("ground")]
        assert len(ground) == 4 and all(len(o.parts[0][0].faces) <= FACES_PER_OBJECT for o in ground), [o.header.name for o in ground]
        assert any("ground cut into 4 pieces" in d for d in conv.report.done) and any("concrete 256x256 halved to 128x128" in d for d in conv.report.done)
        assert conv.textures["concrete"].width == 128 and not conv.report.errors, conv.report.errors
        # the spawn, the zones, the ZIT's box around the park
        assert dat.points[0].kind == W.SPAWN and dat.points[0].name == W.PLAYER_ONE and dat.points[0].position == (0.0, -1.0, -14.0)
        assert len(dat.regions) == 2 and dat.regions[0].hidden == [] and dat.regions[0].mesh == L.name_hash("west")
        tehd = L.walk(L.parse(level["ZIT"]))
        tehd = next(c for c in tehd if c.tag == b"TEHD").data
        low, high = struct.unpack_from("<3f", tehd, 12), struct.unpack_from("<3f", tehd, 24)
        assert low[0] <= -40 and high[0] >= 40 and low[1] <= -3 and high[1] >= 0, (low, high)
        # every strip group within the VU1's 60 entries, every face in a tree
        assert all(len(g.entries) <= W.StripGroup.GROUP_ENTRIES for o in geo.objects for p, _, _ in o.parts for g in p.groups)
        assert all(len(o.collision.faces()) == len(o.parts[0][0].faces) for o in geo.objects if o.collision)
        # the TXR reads as the stock ones do: TXRH id = name hash, an 8-bit BMP each
        for t in L.parse(level["TXR"])[0].kids:
            h = next(k for k in t.kids if k.tag == b"TXRH").data
            name = h[0x14:].split(b"\0")[0].decode()
            assert struct.unpack_from("<I", h, 0)[0] == L.name_hash(name)
            pic = next(k for k in t.kids if k.tag in (b"TBMP", b"TZMP")).data
            assert pic[:2] == b"BM" and struct.unpack_from("<H", pic, 28)[0] == 8
        # the empty script and library have the stock shapes
        assert len(empty_script()) == 60 and struct.unpack_from("<f", empty_script(), 0)[0] == struct.unpack("<f", struct.pack("<f", 2.7))[0]
        assert L.parse(empty_library()[8:])[0].tag == b"DIDF"
        # a sky: its one part has the sky's render class, its instance the sky bit, the Globals the stock fog alpha
        conv, files = convert(model, None, "TUT", "Test Park", sky=os.path.join(os.path.dirname(model), "sky.png"))
        geo = W.Geometry.read(files["GEO"])
        skies = [o for o in geo.objects if o.parts[0][0].flags & 7 == PART_FLAGS_SKY]
        assert len(skies) == 1 and skies[0].header.name == "panorama", [o.header.name for o in skies]
        assert all(o.parts[0][0].flags == PART_FLAGS for o in geo.objects if o is not skies[0])
        sky_instances = [i for i in W.Instances.read(files["INS"]).instances if i.flags & INSTANCE_FLAG_SKY]
        assert len(sky_instances) == 1 and sky_instances[0].geometry_id == skies[0].header.id
        point = next(p for p in W.SceneData.read(files["DAT"]).points if p.kind == W.GLOBALS)
        assert struct.unpack_from("<f", point.tags, 12)[0] == AI_SKY.fog_alpha
        # a prop: its object's field the name's first 31 characters, its word the whole name's hash, which its
        # ZDYN's DDEF has; DynamicsObject's word with AI's class data (DM2's definition: another game's sounds), its
        # instances the stock props' flags and one zero tag word
        conv, files = convert(make_prop_example(os.path.join(tmp, "model")), None, "TUT", "Props")
        geo, ins = W.Geometry.read(files["GEO"]), W.Instances.read(files["INS"])
        props = prop_terms(geo, ins, W)
        assert props == [(EXAMPLE_PROP_NAME[:31], L.name_hash(EXAMPLE_PROP_NAME), True, 2)], props
        prop = next(o for o in geo.objects if o.header.name_hash == L.name_hash(EXAMPLE_PROP_NAME))
        assert prop.type_data == struct.pack("<I", DYNAMICS_OBJECT) + AI_DYNAMICS.class_data
        zdyn = next(k for _, k in geo.other if k.tag == b"ZDYN")
        assert [c.tag for c in zdyn.kids] == [b"DDEF", b"DBDY"] and zdyn.kids[1].data.hex() == EXAMPLE_PROP_DEFINITION["zdyn"][0][1]
        cones = [i for i in ins.instances if i.geometry_id == prop.header.id]
        assert all(i.flags == AI_DYNAMICS.instance_flags and i.tags == AI_DYNAMICS.instance_tags for i in cones)
        assert W.Geometry.read(files["GEO"]).write() == files["GEO"]
        assert all(o.header.name_hash == L.name_hash(o.header.name) for o in geo.objects if o is not prop)
    print("ai_level_convert selftest: ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("model")
    c.add_argument("--out", required=True)
    c.add_argument("--slot", default="TUT", help="the level folder and file name the files take (TUT: the Tutorial's slot)")
    c.add_argument("--level-name")
    c.add_argument("--force", action="store_true", help="write even when a budget the converter cannot fix is over")
    c.add_argument("--sky", help="a PNG drawn as the level's sky (a dome the game keeps around the camera, as the stock panoramas)")
    c.add_argument("--sky-radius", type=float, default=SKY_RADIUS, help="the sky dome's radius (drawn behind everything at any radius; its box counts in the level's world box)")
    c.set_defaults(fn=cmd_convert)
    i = sub.add_parser("info")
    i.add_argument("model")
    i.add_argument("--slot", default="TUT")
    i.add_argument("--level-name")
    i.set_defaults(fn=cmd_info)
    e = sub.add_parser("example")
    e.add_argument("--out", required=True)
    e.set_defaults(fn=cmd_example)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except ValueError as e:
        raise SystemExit("ai_level_convert.py: %s" % e)


if __name__ == "__main__":
    main(sys.argv[1:])
