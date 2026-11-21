#!/usr/bin/env python3
"""Dave Mirra Freestyle BMX 2's Xbox-only levels (Venice, Greenville), park
editor themes (Desert, Highway) and Retro movie made into the PS2 release's
formats, at build time only: the app bundles what this writes and never
reads an Xbox disc or converts anything while DM2 runs.

    python3 -I dm2_xbox.py convert --xbox <DM2 Xbox files> [--ps2 <DM2 PS2 files>] --out <dir> [--level VENICE]
                                   [--max-side 128] [--jobs N] [--texture-budget 2560] [--ffmpeg PATH]
                                   [--only levels,themes,demo,movies]
                                                  the levels, their level-select pictures and
                                                  the Xbox originals of their textures; with
                                                  --ps2, the themes; with ffmpeg, the movie
    python3 -I dm2_xbox.py info <file>...         chunk summary (dm2_level.py's)
    python3 -I dm2_xbox.py ground --dir <dir> --level VENICE --points <file> --out <file> [--step 5]
                                                  the converted level's own collidable surface
                                                  under a grid (dm2_level.py's reference)
    python3 -I dm2_xbox.py edges --dir <dir> --level VENICE --out <file>
                                                  its grindable edges, for the runtime's
                                                  ZX_DM2_XBOX_LEVEL_GRINDS=<file>:<out>
    python3 -I dm2_xbox.py compare <ground out> <probe out>
                                                  the level's own ground (ground's --out)
                                                  against what DM2's query found there (the
                                                  runtime's ZX_DM2_XBOX_LEVEL_PROBE=<points>:<out>)
    python3 -I dm2_xbox.py selftest               the conversions on hand-made records

--xbox is the Xbox disc's files as tools/xbox_files.py extracts them (its
root, media or media/assets). convert writes <out>/<LEVEL>/<LEVEL>.{ZIT,GEO,
INS,TXR,DAT}, <out>/<LEVEL>/<LEVEL>.XTP (the Xbox pictures, for the GPU
renderer: runtime/ps2_texture_replace.h's pack), <out>/<LEVEL>/INFO.TXT (the
level select's text, the Xbox string table's), <out>/CAREER.BIN (the
career's goals, gaps, rewards and texts from default.xbe:
dm2_xbox_career.py), <out>/PICTURES.ZIF (the
level select's pictures, the Xbox UILIB's own) and <out>/levels.txt ("<LEVEL>
<display name>" per line). The runtime (game_overrides/dave_mirra_xboxlevel.cpp,
ZX_DM2_XBOX_LEVELS_DIR) serves them in Woodward Camp's slot from the level
select's extra entries.

With --ps2 (the PS2 disc's files, its root or ASSETS), <out>/themes: the
park editor's Desert and Highway (editor/leDesert, leHighWa: alpha exports,
already the PS2's formats, which every release's SCENE.TXT declares and no
THEMES.TXT lists) as <FOLDER>/<FOLDER>.{ZIT,GEO,INS,TXR,DAT} (raised
THEME_RAISE, with the PS2's Beach theme's light and Globals points: see
THEME_DONOR), THEMES.TXT (their THEME blocks: Beach's with their NAME and
FILE), names.txt and PICTURES.ZIF (each one's theme select picture, drawn
from its files: theme_picture). With ffmpeg (--ffmpeg or PATH),
<out>/movies/ASSETS/MOVIE/RETRO.PSS: the Xbox's movie/retro.bik as the PS2
gallery's retro.pss (retro_movie). The runtime serves them behind their
settings (game_overrides/dave_mirra_xboxthemes.cpp, dave_mirra_gallery.cpp).

What the Xbox release has (and the PS2 release lacks):
- ASSETS/LEVELS/VENICE and GREENVIL: complete levels (GEO, INS, TXR, DAT,
  ZIT; the TXT is the exporter's log). The PS2 SCENE.TXT already names them
  (Level08 venice, Level09 greenvil) and its string table has a "Greenville"
  entry, but the PS2 disc has no files and its per-level tables (stats,
  records, far clips, goals, gaps) stop at eight levels and the park editor.
  The Xbox executable's level select has them as values 8 and 9 (park
  editor 10), with their own goals and gaps in its code.
- The GameCube release has the same two levels (DXT1 textures, no alpha,
  slightly older geometry); the Xbox's are converted.
- riders/LUNCHLAD and riders/EIGHTIES (both releases): not riders. Each is a
  copy of Dave Mirra's export folder (Bike.TXT logs "riders\\DMIRRA\\Bike.ZIT",
  bluemira.ZIT) holding an early Dave Mirra model (Player.ZIT, textures
  MIRRAID5V1 and Mirra_streetID*, byte-identical to riders/riders/AMISHBOY's
  and MDIAS's leftovers) and an older test rider (Player.GEO/TXR); neither
  executable names them, SCENE.TXT lists Rider00..16 as on the PS2. There is
  nothing to convert.

The Xbox's level files are DM2's own engine version's (ZGE2), so the chunk
formats are the PS2's; the differences are the platforms' (found by
comparing the eight levels both releases have):

- TXR: TXRC {TXRH, TDDS: u32 0 + a DDS file} with DXT3 (one A8R8G8B8
  reflection texture a level). The PS2's: TBMP, an 8-bit (or 4-bit) BMP with
  a 256-colour palette whose alpha is 0, then smaller mip levels as more
  TBMPs with the same palette; transparency is a colour key (magenta,
  R >= 248, G <= 8, B >= 248; 159 of the PS2 levels' 1,126 textures have
  it). Made here: the largest DDS level at most --max-side a side (128, the
  PS2 levels' size: 1,034 of 1,126 are 128 or less; the Xbox's 256 ones
  become their own 128 mip), the DDS's next levels as the mips (4 levels at
  most, down to 8 a side), one palette for all levels (median cut over
  colours binned to 6 bits a channel), alpha under 128 as the key when the
  texture's alpha cuts it out (3% of its texels under 64), else opaque.
  The Xbox picture itself goes into the .XTP keyed by the converted
  texture's size and indices, so the GPU renderer draws the original.
- GEO, TGFM (strip groups): the Xbox exporter writes one strip a group
  (21 entries at most; the first index often doubled to keep the winding);
  the PS2's packs strips into groups of up to 60 entries (its VU1 buffer;
  each group is a DMA packet, so 22,680 groups instead of about 3,000 would
  cost VIF1 and VU1 time). Packed here, whole strips in their order, per
  submesh; TGPH +8 (group count) updated. The doubled index draws a
  degenerate triangle, as on the Xbox.
- GEO, TGPT: the later exporter (bmx2_xcube) writes the streams in another
  order (TGVC TGVN TGVU TGFN TGVP TGFP TGFM); put in the PS2's (TGPH TGVP
  TGVN TGVC TGVU TGFP TGFN TGFM TGFE TGFT).
- GEO, MATI +0 (flags): bit 1 (the PS2 levels' only bit; the Xbox sets it on
  more fences and cloth) is kept; bits 0 and 2+ are the material type (MATI
  flags >> 2; 4 = the Xbox's grass, 8 its others) whose VU1 effect routines
  the PS2's microcode lacks: cleared. The layers' render state is the PS2's
  (96.6% of the eight levels' layers identical); their clamp regions (GS
  CLAMP in words 0-1: WMS, WMT, MINU, MAXU, MINV, MAXV) follow the texture's
  new size where they are region modes.
- GEO, levels of detail: Venice and Greenville props have two (TGLO {range,
  0, part id} after each part). DM2 drawing a level object's second LOD
  overwrote VU1 micro memory (convert_lods): the first (the
  detailed one) is kept with the longest range.
- GEO, TGTD (object class): AnimPathFollower (Venice's boat on a path) and
  riderMod (Greenville's cannon) are Xbox classes the PS2 executable does not
  register: Terrain.
- DAT: the CubeMap data point (the Xbox's reflection map) has no PS2 class:
  dropped. Spawns, Globals, Waypoints are the PS2's records.
- INS: the PS2's. ZIT: the four names written as "<LEVEL>.<EXT>".

Python 3.8+, standard library only. Never commit what this writes: it is
your disc's data.
"""
import argparse
import os
import shutil
import struct
import sys

sys.dont_write_bytecode = True  # no __pycache__ in the tools folder
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dm2_xbox_career  # noqa: E402  (CAREER.BIN: the career tables from default.xbe)
import dm2_level  # noqa: E402  (chunks, tag buffers, median cut, BMP writer, collision reference)

from dm2_level import Chunk, parse, serialize, walk, name_hash  # noqa: E402

# Output folder: (Xbox folder, display name). The Xbox's level select names.
LEVELS = {
    "VENICE": ("Venice", "Venice, Italy"),
    "GREENVIL": ("GREENVIL", "Greenville, NC"),
}
# The Xbox level select's values (KSTR_LEVELNAMEMENU_<v>, KSTR_LEVELINFOMENU_<v>,
# its UILIB's "level<v>" pictures).
XBOX_VALUES = {"VENICE": 8, "GREENVIL": 9}

# The park editor's Xbox-only themes (editor/<Xbox folder>): output folder ->
# (Xbox folder, the theme select's name). Alpha exports
# (art_in_progress\Level_Geometry\Park_Editor_Alpha_Exports\desert.max,
# highway.max) that every release's SCENE.TXT declares and no THEMES.TXT lists.
THEMES = {
    "LEDESERT": ("leDesert", "Desert"),
    "LEHIGHWA": ("leHighWa", "Highway"),
}
# What a theme has that they lack is the stock theme Beach's (LEBEACH): the
# 32 x 32 outdoor theme whose frame is theirs (a 20 x 20 floor at columns and
# rows 6 to 25 inside a ring of the theme's own ground six cells wide).
THEME_DONOR = "LEBEACH"
THEME_DONOR_FILE = "LEBeach"  # its FILE in THEMES.TXT
# Their ring ground lies at y 0, the level-0 ground; every shipped theme's at
# level 10 or 15 (y -10, -15: +y is down), and a MAP floor cell needs a level
# over 0. Raised to Beach's 15, so Beach's MAP fits them cell for cell.
THEME_RAISE = 15.0
# Beach's scene points that light a theme: its EditLightCone and EditLightOmni
# points (the relight's park lights, collectThemeLights) and its Globals (the
# scene light's, clear and fog colours, the fog's start and far clip).
LIGHT_POINT_CLASSES = {name_hash("EditLightCone"), name_hash("EditLightOmni"), name_hash("Globals")}

MAX_SIDE = 128
# A level's texels at most this many KB (--texture-budget; 0: none). DM2's
# texture cache (FUN_00106EA8, AI's FUN_00107C88 instruction for instruction)
# gives a frame's textures GS memory from block 5632 up and checks neither the
# CLUTs above nor the end (block 16384). At 128 a side (5.4 MB of texels) DM2's
# own Venice and Greenville ran over the CLUTs in 70 and 47 of about 11,000
# frames and past the end 3 times each (the GS memory audit, ZX_GSMEM_AUDIT,
# runtime/ps2_gs_texture_audit.h; 32 rides spread over each level); at 2560
# (2.5 MB, the PS2 levels' size) they peak at block 13,828 and 13,480.
TEXTURE_BUDGET_KB = 2560
MAX_LEVELS = 4
MIN_MIP_SIDE = 8
KEY = (255, 0, 255, 0)  # the PS2 textures' colour key (palette alpha 0, as theirs)
GROUP_ENTRIES = 60
PS2_PART_ORDER = (b"TGPH", b"TGVP", b"TGVN", b"TGVC", b"TGVU", b"TGFP", b"TGFN", b"TGFM", b"TGFE", b"TGFT")

TERRAIN = name_hash("Terrain")
# The classes DM2's PS2 executable makes objects of (its registry from
# FUN_001C5978, filled by each class's register function; "Prop" is the
# 0x03BE8154 hash all levels use) plus Terrain.
PS2_CLASSES = {name_hash(n) for n in ("Terrain", "Car", "TrafficLight", "Glass", "Exploder", "Mover", "Fountain",
                                      "WaterSurface", "Boat", "DynamicsObject")} | {0x03BE8154}
XBOX_CLASSES = {name_hash("AnimPathFollower"): "AnimPathFollower", name_hash("riderMod"): "riderMod"}
CUBEMAP = name_hash("CubeMap")


def say(text):
    print("[dm2xbox] " + text, flush=True)


# ---- files --------------------------------------------------------------------

class XboxFiles:
    """The extracted Xbox disc (root, media or media/assets), case-blind."""

    def __init__(self, path):
        for sub in ("media/assets", "assets", ""):
            at = self.find(path, sub) if sub else path
            if at and self.find(at, "levels"):
                self.root = at
                return
        raise ValueError("%s has no media/assets/levels (the DM2 Xbox disc's files)" % path)

    @staticmethod
    def find(at, rel):
        for part in rel.split("/"):
            if not os.path.isdir(at):
                return None
            names = {n.upper(): n for n in os.listdir(at)}
            if part.upper() not in names:
                return None
            at = os.path.join(at, names[part.upper()])
        return at

    def read(self, rel):
        at = self.find(self.root, rel)
        if not at or not os.path.isfile(at):
            return None
        with open(at, "rb") as f:
            return f.read()


class Ps2Files:
    """The PS2 disc's files (its root, or ASSETS), case-blind; read(rel) is below ASSETS."""

    def __init__(self, path):
        at = XboxFiles.find(path, "ASSETS") or path
        if not XboxFiles.find(at, "EDITOR"):
            raise ValueError("%s has no ASSETS/EDITOR (the DM2 PS2 disc's files)" % path)
        self.root = at

    def read(self, rel):
        at = XboxFiles.find(self.root, rel)
        if not at or not os.path.isfile(at):
            return None
        with open(at, "rb") as f:
            return f.read()


# ---- textures ---------------------------------------------------------------------

def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def dxt_decode(raw, w, h, kind):
    """RGBA rows (top first) of a DXT1 / DXT3 level."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    step = 8 if kind == b"DXT1" else 16
    rows = [[None] * (bw * 4) for _ in range(bh * 4)]
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * step
            if kind == b"DXT3":
                alpha = int.from_bytes(raw[o:o + 8], "little")
                o += 8
            c0, c1, bits = struct.unpack_from("<HHI", raw, o)
            p0, p1 = rgb565(c0), rgb565(c1)
            if kind == b"DXT1" and c0 <= c1:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            for i in range(16):
                c = pal[(bits >> (2 * i)) & 3]
                if kind == b"DXT3":
                    a = (alpha >> (4 * i)) & 15
                    c = c[:3] + (a * 17,)
                rows[by * 4 + i // 4][bx * 4 + i % 4] = c
    return [row[:w] for row in rows[:h]]


def argb_decode(raw, w, h):
    """RGBA rows of an A8R8G8B8 level (B, G, R, A bytes)."""
    return [[(raw[(y * w + x) * 4 + 2], raw[(y * w + x) * 4 + 1], raw[(y * w + x) * 4], raw[(y * w + x) * 4 + 3])
             for x in range(w)] for y in range(h)]


class Dds:
    """A DDS file's levels (DXT1, DXT3, A8R8G8B8)."""

    def __init__(self, data):
        if data[:4] != b"DDS ":
            raise ValueError("not a DDS file")
        self.h, self.w = struct.unpack_from("<II", data, 12)
        self.mips = max(1, struct.unpack_from("<I", data, 28)[0])
        fourcc = data[84:88]
        bits, masks = struct.unpack_from("<I", data, 88)[0], struct.unpack_from("<4I", data, 92)
        if fourcc in (b"DXT1", b"DXT3"):
            self.kind = fourcc
        elif fourcc == b"\0\0\0\0" and bits == 32 and masks == (0xFF0000, 0xFF00, 0xFF, 0xFF000000):
            self.kind = b"ARGB"
        else:
            raise ValueError("DDS format %r (%d bits)" % (fourcc, bits))
        self.data = data
        self.offsets = []
        at, w, h = 128, self.w, self.h
        for _ in range(self.mips):
            size = self.level_bytes(w, h)
            if at + size > len(data):
                break
            self.offsets.append((at, size, w, h))
            at += size
            w, h = max(1, w // 2), max(1, h // 2)
        self.mips = len(self.offsets)

    def level_bytes(self, w, h):
        if self.kind == b"ARGB":
            return w * h * 4
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if self.kind == b"DXT1" else 16)

    def level(self, k):
        at, size, w, h = self.offsets[k]
        raw = self.data[at:at + size]
        return w, h, (argb_decode(raw, w, h) if self.kind == b"ARGB" else dxt_decode(raw, w, h, self.kind))


def halve(rows):
    """2x2 box filter (alpha-weighted colour), integer arithmetic."""
    h, w = len(rows), len(rows[0])
    out = []
    for y in range(0, h - 1, 2):
        line = []
        for x in range(0, w - 1, 2):
            px = (rows[y][x], rows[y][x + 1], rows[y + 1][x], rows[y + 1][x + 1])
            a = sum(p[3] for p in px)
            if a:
                c = tuple((sum(p[k] * p[3] for p in px) + a // 2) // a for k in range(3))
            else:
                c = tuple((sum(p[k] for p in px) + 2) // 4 for k in range(3))
            line.append(c + ((a + 2) // 4,))
        out.append(line)
    return out


def is_key_colour(c):
    return c[0] >= 248 and c[1] <= 8 and c[2] >= 248


def bin6(c):
    """A colour binned to 6 bits a channel (the median cut works on these),
    moved off the key if it would read as one."""
    r, g, b = c[0] & 0xFC | 2, c[1] & 0xFC | 2, c[2] & 0xFC | 2
    if is_key_colour((r, g, b)):
        g = 10
    return (r, g, b, 0)


def convert_texture(job):
    """One texture: (TXRC chunk bytes of the PS2's, report, pack entry or None).
    job = (TXRH bytes, DDS bytes, max side, opaque: its alpha is not
    transparency)."""
    txrh, dds_bytes, max_side, opaque = job
    tid = struct.unpack_from("<I", txrh, 0)[0]
    name = txrh[0x14:].split(b"\0")[0].decode("latin1")
    dds = Dds(dds_bytes)
    first = 0
    while first + 1 < dds.mips and max(dds.offsets[first][2], dds.offsets[first][3]) > max_side:
        first += 1
    w, h, base = dds.level(first)
    shrunk = 0
    while max(w, h) > max_side and w > 1 and h > 1:
        base = halve(base)
        w, h = len(base[0]), len(base)
        shrunk += 1
    levels = [base]
    k = first + 1
    while len(levels) < MAX_LEVELS and not shrunk and k < dds.mips:
        lw, lh, rows = dds.level(k)
        if min(lw, lh) < MIN_MIP_SIDE:
            break
        levels.append(rows)
        k += 1
    texels = w * h
    low = sum(1 for row in base for c in row if c[3] < 64)
    cutout = not opaque and low * 100 >= texels * 3
    counts = {}
    mapped = []
    for rows in levels:
        out = []
        for row in rows:
            line = []
            for c in row:
                b = KEY if cutout and c[3] < 128 else bin6(c)
                counts[b] = counts.get(b, 0) + 1
                line.append(b)
            out.append(line)
        mapped.append(out)
    keyed = KEY in counts
    if keyed:
        del counts[KEY]
    palette, index = dm2_level.median_cut(counts, 255 if keyed else 256) if counts else ([], {})
    if keyed:
        palette = [KEY] + palette
        index = {c: i + 1 for c, i in index.items()}
        index[KEY] = 0
    kids = [Chunk(b"TXRH", txrh[:4] + struct.pack("<HH", w, h) + txrh[8:])]
    indices = None
    for rows in mapped:
        ind = [[index[c] for c in row] for row in rows]
        if indices is None:
            indices = bytes(i for row in ind for i in row)
        bmp = dm2_level.bmp8(ind, palette)
        kids.append(Chunk(b"TBMP", bmp + b"\0" * (-len(bmp) % 4)))
    txrc = serialize([Chunk(b"TXRC", kids=kids)])
    report = {"id": tid, "name": name, "xbox": (dds.w, dds.h), "ps2": (w, h), "levels": len(levels),
              "cutout": cutout, "keyed": keyed, "kind": dds.kind.decode(), "opaque": opaque}
    # The Xbox picture for the GPU renderer: DXT3 levels as they are (BC2),
    # else its first level as RGBA8.
    if dds.kind == b"DXT3":
        at = dds.offsets[0][0]
        end = dds.offsets[-1][0] + dds.offsets[-1][1]
        picture = (dds.w, dds.h, dds.mips, 1, dds_bytes[at:end])
    else:
        _, _, rows = dds.level(0)
        picture = (dds.w, dds.h, 1, 2, bytes(v for row in rows for c in row for v in c))
    entry = (w, h, fnv1a64(indices), picture, name)
    return txrc, report, entry


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def write_pack(entries):
    """runtime/ps2_texture_replace.h's pack (.xtp): "PXTP", version 1, the
    entries (64 bytes each), then the pictures."""
    head = struct.pack("<4sIII", b"PXTP", 1, len(entries), 0)
    table, blob = b"", b""
    base = 16 + 64 * len(entries)
    for tw, th, key, (pw, ph, mips, fmt, data), name in entries:
        table += struct.pack("<HHBBHQHHBBHII32s", tw, th, 8, 0, 0, key, pw, ph, mips, fmt, 0, base + len(blob),
                             len(data), name.encode("latin1")[:32].ljust(32, b"\0"))
        blob += data
        blob += b"\0" * (-len(blob) % 16)
    return head + table + blob


def dds_of(txrc):
    head = next(k for k in txrc.kids if k.tag == b"TXRH")
    dds = next((k for k in txrc.kids if k.tag == b"TDDS"), None)
    return head.data, (dds.data[4:] if dds else None)


def material_textures(geo):
    """{texture id: set of MATI flags of the materials using it}."""
    out = {}
    for c in walk(parse(geo)):
        if c.tag == b"MATI":
            flags = struct.unpack_from("<I", c.data, 0)[0]
            for i in range(struct.unpack_from("<I", c.data, 0x28)[0]):
                out.setdefault(struct.unpack_from("<I", c.data, 0x2C + 28 * i + 0x10)[0], set()).add(flags)
    return out


def ps2_texels(dds, max_side):
    """The texels (bytes, 8-bit) convert_texture makes of a DDS at a max side:
    its first level and the mips it keeps."""
    first = 0
    while first + 1 < dds.mips and max(dds.offsets[first][2], dds.offsets[first][3]) > max_side:
        first += 1
    w, h = dds.offsets[first][2], dds.offsets[first][3]
    shrunk = 0
    while max(w, h) > max_side and w > 1 and h > 1:
        w, h, shrunk = w // 2, h // 2, shrunk + 1
    total, levels, k = w * h, 1, first + 1
    while levels < MAX_LEVELS and not shrunk and k < dds.mips:
        lw, lh = dds.offsets[k][2], dds.offsets[k][3]
        if min(lw, lh) < MIN_MIP_SIDE:
            break
        total, levels, k = total + lw * lh, levels + 1, k + 1
    return total


def budget_sides(ddss, max_side, budget):
    """Each texture's max side so that all of them come to at most `budget`
    texel bytes: the largest made one level smaller first (ties by order),
    none below 16 a side."""
    sides = [max_side] * len(ddss)
    sizes = [ps2_texels(d, max_side) for d in ddss]
    total = sum(sizes)
    while total > budget:
        best = max((i for i in range(len(ddss)) if sides[i] > 16), key=lambda i: (sizes[i], -i), default=None)
        if best is None:
            break
        sides[best] //= 2
        new = ps2_texels(ddss[best], sides[best])
        total += new - sizes[best]
        sizes[best] = new
    return sides, total


def convert_txr(data, max_side, jobs, report, opaque=frozenset(), budget=0):
    """The level's TXR as the PS2's; (TXR bytes, texture sizes {id: (xbox, ps2)},
    pack entries). `opaque`: ids whose alpha is no transparency (the Xbox's
    grass: its material type draws the base layer opaque and the blades by
    the alpha, which the PS2's microcode cannot). `budget` (bytes, 0: none):
    the texels of all textures at most that (budget_sides)."""
    tree = parse(data)
    work = []
    for c in tree[0].kids:
        if c.tag != b"TXRC":
            raise ValueError("a %r chunk in a texture library" % c.tag)
        head, dds = dds_of(c)
        if dds is None:
            raise ValueError("texture %r has no DDS" % head[0x14:].split(b"\0")[0])
        work.append((head, dds, max_side, struct.unpack_from("<I", head, 0)[0] in opaque))
    if budget:
        sides, total = budget_sides([Dds(w[1]) for w in work], max_side, budget)
        report["textures smaller for the budget"] += sum(1 for side in sides if side < max_side)
        report["texel bytes"] = total
        work = [(h, d, side, o) for (h, d, _, o), side in zip(work, sides)]
    if jobs > 1:
        import multiprocessing
        with multiprocessing.Pool(jobs) as pool:
            results = pool.map(convert_texture, work, chunksize=4)
    else:
        results = [convert_texture(j) for j in work]
    sizes, entries, out = {}, [], b""
    for txrc, r, entry in results:
        out += txrc
        sizes[r["id"]] = (r["xbox"], r["ps2"])
        entries.append(entry)
        report["textures"] += 1
        report["textures smaller"] += r["xbox"] != r["ps2"]
        report["textures keyed (cut out)"] += r["keyed"]
        report["textures opaque (grass)"] += r["opaque"]
        report["textures " + r["kind"]] += 1
    library = b"FORM" + struct.pack("<I", len(out)) + out
    return library, sizes, entries


# ---- geometry -------------------------------------------------------------------

def read_groups(data):
    """TGFM: [(submesh word, [indices])]."""
    groups, at = [], 0
    while at + 4 <= len(data):
        head, count = struct.unpack_from("<HH", data, at)
        groups.append((head, list(struct.unpack_from("<%dH" % count, data, at + 4))))
        at += (count * 2 + 7) & ~3
    if at != len(data):
        raise ValueError("TGFM of %d bytes ends at %d" % (len(data), at))
    return groups


def write_groups(groups):
    out = b""
    for head, idx in groups:
        out += struct.pack("<HH%dH" % len(idx), head, len(idx), *idx)
        out += b"\0" * (-len(out) % 4)
    return out


def strips_of(idx):
    """A group's strips: each starts at an index with 0x8000 that follows one
    without it (a strip starts with two 0x8000 indices)."""
    strips, cur = [], []
    for k, i in enumerate(idx):
        if i & 0x8000 and cur and not cur[-1] & 0x8000:
            strips.append(cur)
            cur = []
        cur.append(i)
    if cur:
        strips.append(cur)
    return strips


def pack_groups(groups, limit=GROUP_ENTRIES):
    """The Xbox's one-strip groups packed into PS2 groups of up to `limit`
    entries (whole strips, in order, per submesh word)."""
    out = []
    for head, idx in groups:
        for strip in strips_of(idx):
            if len(strip) > limit:
                raise ValueError("a strip of %d entries" % len(strip))
            if out and out[-1][0] == head and len(out[-1][1]) + len(strip) <= limit:
                out[-1][1].extend(strip)
            else:
                out.append((head, list(strip)))
    return out


def triangles(groups):
    """The triangles strips draw (an index without 0x8000 ends one with the
    two before it; degenerate ones left out), with each one's parity."""
    tris = []
    for _, idx in groups:
        for k, i in enumerate(idx):
            if i & 0x8000 or k < 2:
                continue
            a, b, c = idx[k - 2] & 0x7FFF, idx[k - 1] & 0x7FFF, i & 0x7FFF
            if a != b and b != c and a != c:
                tris.append((a, b, c))
    return tris


def convert_part(part, report):
    kids = {k.tag: k for k in part.kids}
    extra = [k for k in part.kids if k.tag not in PS2_PART_ORDER]
    if extra:
        raise ValueError("a part with %s" % [k.tag for k in extra])
    part.kids = [kids[t] for t in PS2_PART_ORDER if t in kids]
    groups = read_groups(kids[b"TGFM"].data)
    packed = pack_groups(groups)
    if triangles(packed) != triangles(groups):
        raise ValueError("packing changed the triangles")
    kids[b"TGFM"].data = write_groups(packed)
    head = kids[b"TGPH"]
    if struct.unpack_from("<I", head.data, 8)[0] != len(groups):
        raise ValueError("TGPH says %d groups, TGFM has %d" % (struct.unpack_from("<I", head.data, 8)[0], len(groups)))
    head.data = head.data[:8] + struct.pack("<I", len(packed)) + head.data[12:]
    report["strip groups in"] += len(groups)
    report["strip groups out"] += len(packed)
    report["parts"] += 1
    report["faces"] += struct.unpack_from("<H", head.data, 6)[0]


def scale_clamp(w0, w1, sx, sy):
    """A layer's GS CLAMP fields (word 0 bits 10-23, word 1 bits 0-29) for
    a texture scaled by sx, sy (powers of two): region clamp (2) keeps its
    region, region repeat (3) its mask and fix, in the new texels."""
    wms, wmt = (w0 >> 10) & 3, (w0 >> 12) & 3
    minu, maxu = (w0 >> 14) & 0x3FF, w1 & 0x3FF
    minv, maxv = (w1 >> 10) & 0x3FF, (w1 >> 20) & 0x3FF

    def region(mode, lo, hi, s):
        if mode == 2:
            return int(lo * s), max(int((hi + 1) * s) - 1, int(lo * s))
        if mode == 3:
            return max(int((lo + 1) * s) - 1, 0), int(hi * s)
        return lo, hi
    minu, maxu = region(wms, minu, maxu, sx)
    minv, maxv = region(wmt, minv, maxv, sy)
    w0 = (w0 & ~(0x3FF << 14)) | (minu << 14)
    w1 = (w1 & ~0x3FFFFFFF) | maxu | (minv << 10) | (maxv << 20)
    return w0, w1


def convert_mati(data, sizes, report):
    if len(data) != dm2_level.DM2_MATI:
        raise ValueError("MATI of %d bytes" % len(data))
    m = bytearray(data)
    flags = struct.unpack_from("<I", m, 0)[0]
    if flags & ~2:
        report["material flags cleared (0x%x)" % (flags & ~2)] += 1
    struct.pack_into("<I", m, 0, flags & 2)
    for i in range(struct.unpack_from("<I", m, 0x28)[0]):
        at = 0x2C + 28 * i
        w0, w1 = struct.unpack_from("<2I", m, at)
        tex = struct.unpack_from("<I", m, at + 0x10)[0]
        if (w0 >> 10) & 3 >= 2 or (w0 >> 12) & 3 >= 2:
            (xw, xh), (pw, ph) = sizes.get(tex, ((1, 1), (1, 1)))
            if (xw, xh) != (pw, ph):
                w0, w1 = scale_clamp(w0, w1, pw / xw, ph / xh)
                struct.pack_into("<2I", m, at, w0, w1)
                report["clamp regions scaled"] += 1
    return bytes(m)


def part_id(part):
    return struct.unpack_from("<I", next(k for k in part.kids if k.tag == b"TGPH").data, 0x10)[0]


def convert_lods(obj, report):
    """Props with two levels of detail (TGFR byte 2 = 2, a TGLO {u32 draw
    range, u32, u32 part id} after each LOD's part): DM2's PS2 levels have
    one per object, and DM2 drawing a level object's second LOD (its node's
    part swapped by FUN_00123F30) sent VIF1 a packet that overwrote VU1
    micro memory (garbage drawn from then on). Kept: the first LOD with the
    longest range; the other LOD's TGLO, part and materials dropped."""
    lods = [k for k in obj.kids if k.tag == b"TGLO"]
    if len(lods) < 2:
        return
    reach = max(struct.unpack_from("<f", k.data, 0)[0] for k in lods)
    lods[0].data = struct.pack("<f", reach) + lods[0].data[4:]
    for lod in lods[1:]:
        gone = struct.unpack_from("<I", lod.data, 8)[0]
        kids = obj.kids
        at = next(i for i, k in enumerate(kids) if k.tag == b"TGPT" and part_id(k) == gone)
        end = at + 1
        while end < len(kids) and kids[end].tag in (b"TGLO", b"TMTB"):
            end += 1
        del kids[at:end]
        report["second LODs dropped"] += 1
    for k in obj.kids:
        if k.tag == b"TGFR":
            k.data = k.data[:2] + b"\x01" + k.data[3:]


def drop_unused_materials(obj, report):
    """A part's materials that no strip group draws (TGFM's submesh byte),
    dropped from its TMTB: DM2 builds a part's DMA chain per material and
    CALLs each material's strip packet, which a material without strips
    never gets; its CALL went to whatever the pointer held (vertex data in
    the heap), so VIF1 ran data as VIFcodes (about 2,000 stray interrupts
    a frame, garbage VU1 programs) and lost the rest of the frame's draws,
    the rider's among them. DM2's PS2 levels have no such material. The
    strip groups' and faces' material numbers are renumbered; a face of a
    dropped material takes the part's first one."""
    kids = obj.kids
    for i, part in enumerate(kids):
        if part.tag != b"TGPT":
            continue
        table = None
        for k in kids[i + 1:]:
            if k.tag == b"TGPT":
                break
            if k.tag == b"TMTB":
                table = k
                break
        streams = {k.tag: k for k in part.kids}
        if table is None or b"TGFM" not in streams or b"TGFP" not in streams:
            continue
        strips = bytearray(streams[b"TGFM"].data)
        used, at = set(), 0
        while at + 4 <= len(strips):
            head, count = struct.unpack_from("<HH", strips, at)
            used.add(head >> 8)
            at += (count * 2 + 7) & ~3
        materials = len(table.kids)
        if not used or used == set(range(materials)) or max(used) >= materials:
            continue
        renumber = {old: new for new, old in enumerate(sorted(used))}
        at = 0
        while at + 4 <= len(strips):
            head, count = struct.unpack_from("<HH", strips, at)
            struct.pack_into("<H", strips, at, (renumber[head >> 8] << 8) | (head & 0xFF))
            at += (count * 2 + 7) & ~3
        streams[b"TGFM"].data = bytes(strips)
        faces = bytearray(streams[b"TGFP"].data)
        for f in range(len(faces) // 8):
            flags = struct.unpack_from("<H", faces, 8 * f)[0]
            struct.pack_into("<H", faces, 8 * f, (flags & 0xFF00) | renumber.get(flags & 0xFF, 0))
        streams[b"TGFP"].data = bytes(faces)
        table.kids = [m for n, m in enumerate(table.kids) if n in renumber]
        head = streams[b"TGPH"]
        head.data = head.data[:0xE] + struct.pack("<H", len(table.kids)) + head.data[0x10:]
        report["unused materials dropped"] += materials - len(table.kids)



KEEP_LODS = False  # --keep-lods (developers): the Xbox's second LODs kept


def convert_geo(data, sizes, report, offsets=None):
    """The GEO as the PS2's; offsets (object id -> (x, y, z)) gets the
    objects recentred (convert_ins moves their instances)."""
    tree = parse(data)
    for obj in tree[0].kids:
        if obj.tag != b"TGEO":
            continue
        report["objects"] += 1
        # Past the collision tree's range (s16 hundredths, +-327.67) the
        # exporter wrapped the splits (Greenville's terrain_street017 to x
        # 348, Terrain_newrampclub010 to z -362): DM2's walk then misses
        # the object's faces there. Recentred (dm2_level.py's fix).
        offset = dm2_level.recentre_object(obj)
        if offset is not None:
            report["objects recentred"] += 1
            if offsets is not None:
                offsets[dm2_level.object_head(obj)[1]] = offset
        if not KEEP_LODS:
            convert_lods(obj, report)
        for k in obj.kids:
            if k.tag == b"TGTD" and len(k.data) >= 4:
                cls = struct.unpack_from("<I", k.data, 0)[0]
                if cls not in PS2_CLASSES:
                    report["type %s as Terrain" % XBOX_CLASSES.get(cls, "0x%08x" % cls)] += 1
                    k.data = struct.pack("<I", TERRAIN) + k.data[4:]
        for k in obj.kids:
            if k.tag == b"TGPT":
                convert_part(k, report)
        drop_unused_materials(obj, report)
    for c in walk(tree):
        if c.tag == b"MATI":
            c.data = convert_mati(c.data, sizes, report)
            report["materials"] += 1
    return serialize(tree)


def convert_ins(data, report, offsets):
    """The INS as it is but for the instances of recentred objects (convert_geo),
    moved so that the object stays where it was."""
    if not offsets:
        return data
    tree = parse(data)
    for c in walk(tree):
        if c.tag != b"TGIH":
            continue
        offset = offsets.get(struct.unpack_from("<I", c.data, 0x40)[0])
        if offset is None:
            continue
        if struct.unpack_from("<I", c.data, 0)[0] & 0x400:
            raise ValueError("a recentred object's instance is mirrored (flag 0x400)")
        q, p = struct.unpack_from("<4f", c.data, 8), struct.unpack_from("<3f", c.data, 0x18)
        moved = tuple(a + b for a, b in zip(p, dm2_level.quat_rotate(q, offset)))
        c.data = c.data[:0x18] + struct.pack("<3f", *moved) + c.data[0x24:]
        report["instances moved"] += 1
    return serialize(tree)


def convert_dat(data, report):
    tree = parse(data)
    form = tree[0]
    kept = []
    for c in form.kids:
        if c.tag == b"SDPT" and struct.unpack_from("<I", c.data, 4)[0] == CUBEMAP:
            report["dropped CubeMap"] += 1
            continue
        if c.tag == b"SDPT" and struct.unpack_from("<I", c.data, 4)[0] == dm2_level.SPAWN:
            report["spawns"] += 1
            if struct.unpack_from("<I", c.data, 8)[0] == dm2_level.PLAYER_ONE:
                report["start"] = struct.unpack_from("<3f", c.data, 0x10)
        kept.append(c)
    if "start" not in report:
        raise ValueError("no playerone start point")
    form.kids = kept
    return serialize(tree)


def convert_zit(data, level):
    tree = parse(data)
    for c in walk(tree):
        if c.tag in (b"ZFTX", b"ZFGE", b"ZFIS", b"ZFSD"):
            ext = c.data.split(b"\0")[0].rsplit(b".", 1)[-1].upper()
            name = (level + "." + ext.decode("latin1")).encode("latin1")
            c.data = name + b"\0" * (4 - len(name) % 4)
    return serialize(tree)


# ---- the park editor's themes ----------------------------------------------------

def raise_ins(data, dy, report):
    """Every instance (TGIH: flags, quaternion at +8, position at +0x18) moved dy along y."""
    tree = parse(data)
    for c in walk(tree):
        if c.tag == b"TGIH":
            x, y, z = struct.unpack_from("<3f", c.data, 0x18)
            c.data = c.data[:0x18] + struct.pack("<3f", x, y + dy, z) + c.data[0x24:]
            report["instances raised"] += 1
    return serialize(tree)


def theme_dat(data, donor, dy, report):
    """The theme's scene points moved dy along y (as its instances), then the
    donor's light and Globals points as they are (they sit in the frame both
    share once the theme is raised)."""
    tree = parse(data)
    form = tree[0]
    points = [c for c in form.kids if c.tag == b"SDPT"]
    for c in points:
        x, y, z = struct.unpack_from("<3f", c.data, 0x10)
        c.data = c.data[:0x10] + struct.pack("<3f", x, y + dy, z) + c.data[0x1C:]
        report["points raised"] += 1
    own = {struct.unpack_from("<I", c.data, 4)[0] for c in points}
    if own & LIGHT_POINT_CLASSES:
        raise ValueError("the theme has light or Globals points of its own")
    added = [c for c in parse(donor)[0].kids
             if c.tag == b"SDPT" and struct.unpack_from("<I", c.data, 4)[0] in LIGHT_POINT_CLASSES]
    report["light points added"] += len(added)
    at = max(i for i, c in enumerate(form.kids) if c.tag == b"SDPT") + 1 if points else 0
    form.kids[at:at] = added
    return serialize(tree)


def theme_blocks(text):
    """THEMES.TXT's THEME blocks: [(FILE, block text)]."""
    import re
    out = []
    for m in re.finditer(r"THEME\s*\{.*?\n\s*\}", text, re.S):
        f = re.search(r'FILE\s+"([^"]*)"', m.group(0))
        out.append((f.group(1) if f else "", m.group(0)))
    return out


def theme_block(donor_block, name, folder):
    """The donor's THEME block (its AMBIANT, CURSOR, WIDTH, HEIGHT, BORDER and
    MAP) with the theme's NAME and FILE."""
    import re
    block = re.sub(r'NAME\s+"[^"]*"', 'NAME\t"%s"' % name, donor_block, count=1)
    return re.sub(r'FILE\s+"[^"]*"', 'FILE\t"%s"' % folder, block, count=1)


# The theme select's picture of a theme (UILIB ParkMenu.zif "Themes.<value>":
# 256 x 256, 8 bits, as the stock nine): the theme drawn from above one
# corner of its floor, as theirs are, with its floor of empty cells (its own
# Editobj_tower, a flat 10 x 10 m top on every second cell at the MAP's
# level), its textures lit by its AMBIANT and brightness 5 (the relight's
# colour with no light near), its Globals' clear colour behind.
PICTURE_SIDE = 256
PICTURE_SCALE = 2          # drawn at twice the side, averaged down
PICTURE_EYE = (-62.0, -52.0, -62.0)
PICTURE_TARGET = (8.0, -15.0, 8.0)
PICTURE_FOV = 62.0
BRIGHTNESS = 5             # edit_lights.cpp kDefaultBrightness


def theme_textures(txr):
    """{texture id: (width, height, rows of (r, g, b, a))} of a TXR's TBMPs (the first level), the colour key clear."""
    out = {}
    for c in walk(parse(txr)):
        if c.tag != b"TXRC":
            continue
        head = next((k for k in c.kids if k.tag == b"TXRH"), None)
        pic = next((k for k in c.kids if k.tag == b"TBMP"), None)
        if head is None or pic is None:
            continue
        if struct.unpack_from("<H", pic.data, 28)[0] in (24, 32):
            w, h = struct.unpack_from("<ii", pic.data, 18)
            bpp = struct.unpack_from("<H", pic.data, 28)[0] // 8
            at, stride = struct.unpack_from("<I", pic.data, 10)[0], (w * bpp + 3) // 4 * 4
            rows = [[(pic.data[at + r * stride + x * bpp + 2], pic.data[at + r * stride + x * bpp + 1],
                      pic.data[at + r * stride + x * bpp], 255) for x in range(w)]
                    for r in (range(abs(h) - 1, -1, -1) if h > 0 else range(abs(h)))]
            h = abs(h)
        else:
            w, h, rows = dm2_level.decode_bmp(pic.data, opaque=True)
        rows = [[(0, 0, 0, 0) if is_key_colour(p) else p for p in row] for row in rows]
        out[struct.unpack_from("<I", head.data, 0)[0]] = (w, h, rows)
    return out


def theme_objects(geo):
    """{object id: (name, [(positions, uvs, [(i0, i1, i2, texture id)])])}: each part's faces with the first layer's
    texture of their material (the face's low byte: its object's TMTB entry)."""
    out = {}
    for obj in parse(geo)[0].kids:
        if obj.tag != b"TGEO":
            continue
        head = next(k for k in obj.kids if k.tag == b"TGHD").data
        mats = [struct.unpack_from("<I", m.data, 0x2C + 0x10)[0] if struct.unpack_from("<I", m.data, 0x28)[0] else None
                for t in obj.kids if t.tag == b"TMTB" for m in t.kids if m.tag == b"MATI"]
        parts = []
        for part in (k for k in obj.kids if k.tag == b"TGPT"):
            s = {k.tag: k.data for k in part.kids}
            nv, nf = struct.unpack_from("<HH", s[b"TGPH"], 4)
            if b"TGVU" not in s or len(s[b"TGVU"]) < 8 * nv:
                continue
            pos = [struct.unpack_from("<3f", s[b"TGVP"], 12 * i) for i in range(nv)]
            uv = [struct.unpack_from("<2f", s[b"TGVU"], 8 * i) for i in range(nv)]
            faces = []
            for i in range(nf):
                f = struct.unpack_from("<4H", s[b"TGFP"], 8 * i)
                m = f[0] & 0xFF
                if max(f[1:]) < nv and m < len(mats) and mats[m] is not None:
                    faces.append((f[1], f[2], f[3], mats[m]))
            parts.append((pos, uv, faces))
            break  # the first level of detail
        out[struct.unpack_from("<I", head, 0x18)[0]] = (head[0x1C:0x3C].split(b"\0")[0].decode("latin1"), parts)
    return out


def theme_picture(geo, ins, txr, dat, block):
    """The theme select's picture of a theme (PICTURE_*): rows of (r, g, b)."""
    import math
    import re
    textures = theme_textures(txr)
    objects = theme_objects(geo)
    ambient = [int(x) for x in re.search(r"AMBIANT\s+(\d+)\s+(\d+)\s+(\d+)", block).groups()]
    light = [min(255, a + BRIGHTNESS) / 128.0 for a in ambient]
    width = int(re.search(r"WIDTH\s+(\d+)", block).group(1))
    height = int(re.search(r"HEIGHT\s+(\d+)", block).group(1))
    cells = [int(x) for x in re.search(r"MAP\s+([-\d\s]+)", block).group(1).split()[:width * height]]
    clear = (0, 0, 0)
    for c in parse(dat)[0].kids:
        if c.tag == b"SDPT" and struct.unpack_from("<I", c.data, 4)[0] == name_hash("Globals"):
            clear = tuple(c.data[0x30:0x33])  # the second colour tag: the clear colour
    # What is drawn: every instance's object, then a tower on every tower cell of the floor (MAP > 0).
    placed = []
    instances = [c.data for c in walk(parse(ins)) if c.tag == b"TGIH"]
    by_id = {struct.unpack_from("<I", d, 0x48)[0]: d for d in instances}
    for d in instances:
        chain = []
        at = d
        for _ in range(16):
            chain.append((struct.unpack_from("<4f", at, 8), struct.unpack_from("<3f", at, 0x18)))
            at = by_id.get(struct.unpack_from("<I", at, 0x44)[0])
            if at is None:
                break
        obj = objects.get(struct.unpack_from("<I", d, 0x40)[0])
        if obj:
            placed.append((obj[1], chain))
    tower = next((o for o in objects.values() if o[0].lower() == "editobj_tower"), None)
    if tower:
        for row in range(0, height, 2):
            for col in range(1, width, 2):
                level = cells[row * width + col] if row * width + col < len(cells) else 0
                if level > 0:
                    pos = (col * 5.0 - (width - 1) * 2.5, -float(level), row * 5.0 - (height - 1) * 2.5)
                    placed.append((tower[1], [((0.0, 0.0, 0.0, 1.0), pos)]))
    # The camera: +y is down; the picture's x right, y down.
    side = PICTURE_SIDE * PICTURE_SCALE
    ex, ey, ez = PICTURE_EYE
    fx, fy, fz = (t - e for t, e in zip(PICTURE_TARGET, PICTURE_EYE))
    n = math.sqrt(fx * fx + fy * fy + fz * fz)
    fx, fy, fz = fx / n, fy / n, fz / n
    rx, ry, rz = -fz, 0.0, fx                      # right: forward x down (0, 1, 0)... level with the ground
    n = math.sqrt(rx * rx + rz * rz)
    rx, rz = rx / n, rz / n
    ux, uy, uz = ry * fz - rz * fy, rz * fx - rx * fz, rx * fy - ry * fx  # down in the picture
    focal = side / 2 / math.tan(math.radians(PICTURE_FOV) / 2)
    near = 0.5
    depth = [float("inf")] * (side * side)
    colour = [clear] * (side * side)

    def project(p):
        dx, dy, dz = p[0] - ex, p[1] - ey, p[2] - ez
        z = dx * fx + dy * fy + dz * fz
        return (side / 2 + focal * (dx * rx + dy * ry + dz * rz) / z if z > near else None,
                side / 2 + focal * (dx * ux + dy * uy + dz * uz) / z if z > near else None, z)

    for parts, chain in placed:
        for pos, uv, faces in parts:
            world = []
            for v in pos:
                for q, t in chain:
                    v = tuple(a + b for a, b in zip(dm2_level.quat_rotate(q, v), t))
                world.append(project(v))
            for i0, i1, i2, tex in faces:
                a, b, c = world[i0], world[i1], world[i2]
                if a[0] is None or b[0] is None or c[0] is None:
                    continue
                texture = textures.get(tex)
                if texture is None:
                    continue
                tw, th, trows = texture
                area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
                if abs(area) < 1e-9:
                    continue
                x0, x1 = max(0, int(min(a[0], b[0], c[0]))), min(side - 1, int(max(a[0], b[0], c[0])) + 1)
                y0, y1 = max(0, int(min(a[1], b[1], c[1]))), min(side - 1, int(max(a[1], b[1], c[1])) + 1)
                if x0 > x1 or y0 > y1:
                    continue
                # Perspective-correct texture coordinates (u/z, v/z, 1/z across the face).
                ia, ib, ic = 1.0 / a[2], 1.0 / b[2], 1.0 / c[2]
                ua, va = uv[i0][0] * ia, uv[i0][1] * ia
                ub, vb = uv[i1][0] * ib, uv[i1][1] * ib
                uc, vc = uv[i2][0] * ic, uv[i2][1] * ic
                for y in range(y0, y1 + 1):
                    py = y + 0.5
                    for x in range(x0, x1 + 1):
                        px = x + 0.5
                        w0 = ((b[0] - px) * (c[1] - py) - (b[1] - py) * (c[0] - px)) / area
                        w1 = ((c[0] - px) * (a[1] - py) - (c[1] - py) * (a[0] - px)) / area
                        w2 = 1.0 - w0 - w1
                        if w0 < 0 or w1 < 0 or w2 < 0:
                            continue
                        iz = w0 * ia + w1 * ib + w2 * ic
                        z = 1.0 / iz
                        k = y * side + x
                        if z >= depth[k]:
                            continue
                        u = (w0 * ua + w1 * ub + w2 * uc) * z
                        v = (w0 * va + w1 * vb + w2 * vc) * z
                        p = trows[int(v * th) % th][int(u * tw) % tw]
                        if p[3] == 0:
                            continue
                        depth[k] = z
                        colour[k] = (min(255, int(p[0] * light[0])), min(255, int(p[1] * light[1])),
                                     min(255, int(p[2] * light[2])))
    s = PICTURE_SCALE
    rows = []
    for y in range(PICTURE_SIDE):
        row = []
        for x in range(PICTURE_SIDE):
            px = [colour[(y * s + j) * side + x * s + i] for j in range(s) for i in range(s)]
            row.append(tuple(sum(p[ch] for p in px) // len(px) for ch in range(3)))
        rows.append(row)
    return rows


def write_png(path, rows):
    import zlib
    w, h = len(rows[0]), len(rows)
    raw = b"".join(b"\0" + bytes(c for p in row for c in p[:3]) for row in rows)

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def stock_theme_picture(ps2):
    """A stock theme's picture asset in UILIB.ZAL's ParkMenu.zif (the library whose assets are named "0" on): the
    form ours take."""
    uilib = ps2.read("UILIB.ZAL")
    for entry in dm2_level.zal_entries(uilib or b"ZDIR"):
        if entry[:4] != b"ULIB":
            continue
        try:
            lib = parse_ui(entry)
        except ValueError:
            continue
        for asset in (a for c in lib if c.tag == b"ULIB" for a in c.kids):
            if asset.tag == b"UAST" and asset.kids and asset.kids[0].tag == b"HAST" and \
                    asset.kids[0].data[0x80:0xA0].split(b"\0")[0] == b"0" and b"Themes" in asset.kids[0].data[:0x80]:
                return asset
    raise ValueError("UILIB.ZAL has no theme pictures (ParkMenu.zif)")


def theme_picture_asset(stock, rows, name):
    """The stock picture's asset with our picture, named `name` (its HAST name and its texture's, the hash with
    it: what "Themes.<value>" finds), the palette by median cut."""
    import copy
    from collections import Counter
    counts = Counter((r & 0xFC, g & 0xFC, b & 0xFC, 255) for row in rows for r, g, b in row)
    palette, index = dm2_level.median_cut(counts, 256)
    indices = [[index[(r & 0xFC, g & 0xFC, b & 0xFC, 255)] for r, g, b in row] for row in rows]
    bmp = dm2_level.bmp8(indices, [(r, g, b, 0) for r, g, b, _ in palette])
    raw = name.encode("latin1")
    asset = copy.deepcopy(stock)
    hast = asset.kids[0].data
    asset.kids[0].data = (b"dm2_xbox.py\\" + raw + b".bmp").ljust(0x80, b"\0") + raw.ljust(32, b"\0") + hast[0xA0:]
    for c in walk(asset.kids):
        if c.tag == b"TXRH":
            c.data = struct.pack("<IHH", name_hash(name), PICTURE_SIDE, PICTURE_SIDE) + c.data[8:0x14] + \
                raw.ljust(32, b"\0") + c.data[0x34:]
        elif c.tag == b"TBMP":
            c.data = bmp
    return asset


def convert_themes(xbox, ps2, out_dir, pictures=True, picture_dir=None):
    """themes/<FOLDER>/<FOLDER>.{ZIT,GEO,INS,TXR,DAT}, themes/THEMES.TXT (their
    THEME blocks, for after the PS2's) and themes/names.txt ("<FOLDER> <donor's
    FILE> <name>" per line, in the theme select's order: the runtime gives a
    theme the donor's park sound bank)."""
    from collections import Counter
    themes_txt = ps2.read("EDITOR/THEMES.TXT")
    donor_dat = ps2.read("EDITOR/%s/%s.DAT" % (THEME_DONOR, THEME_DONOR))
    if themes_txt is None or donor_dat is None:
        raise ValueError("the PS2 files have no EDITOR/THEMES.TXT or EDITOR/%s" % THEME_DONOR)
    donor = dict(theme_blocks(themes_txt.decode("latin1"))).get(THEME_DONOR_FILE)
    if donor is None:
        raise ValueError("THEMES.TXT has no %s block" % THEME_DONOR_FILE)
    stock_count = len(theme_blocks(themes_txt.decode("latin1")))
    stock_picture = stock_theme_picture(ps2) if pictures else None
    blocks, lines, assets = [], [], []
    for folder, (xbox_folder, name) in THEMES.items():
        files = {}
        for ext in ("ZIT", "GEO", "INS", "TXR", "DAT"):
            data = xbox.read("editor/%s/%s.%s" % (xbox_folder, xbox_folder, ext))
            if data is None:
                raise ValueError("editor/%s/%s.%s is not in the Xbox files" % (xbox_folder, xbox_folder, ext))
            files[ext] = data
        report = Counter()
        made = {
            "ZIT": convert_zit(files["ZIT"], folder),
            "GEO": files["GEO"],  # the PS2's chunk set, TBMP textures: as they are
            "INS": raise_ins(files["INS"], -THEME_RAISE, report),
            "TXR": files["TXR"],
            "DAT": theme_dat(files["DAT"], donor_dat, -THEME_RAISE, report),
        }
        to = os.path.join(out_dir, folder)
        os.makedirs(to, exist_ok=True)
        for ext, data in made.items():
            with open(os.path.join(to, "%s.%s" % (folder, ext)), "wb") as f:
                f.write(data)
        blocks.append(theme_block(donor, name, folder))
        if pictures:
            import time
            t0 = time.time()
            rows = theme_picture(made["GEO"], made["INS"], made["TXR"], made["DAT"], blocks[-1])
            value = str(stock_count + len(assets))
            assets.append(theme_picture_asset(stock_picture, rows, value))
            if picture_dir:
                write_png(os.path.join(picture_dir, "%s.png" % folder), rows)
            say("theme %s: picture Themes.%s drawn (%.0f s)" % (folder, value, time.time() - t0))
        lines.append("%s %s %s" % (folder, THEME_DONOR_FILE, name))
        say("theme %s (%s): %d instances and %d points raised %.0f m, %d light points from %s" % (
            folder, name, report["instances raised"], report["points raised"], THEME_RAISE,
            report["light points added"], THEME_DONOR))
    with open(os.path.join(out_dir, "THEMES.TXT"), "w", newline="\r\n") as f:
        f.write("\n\n".join(blocks) + "\n")
    with open(os.path.join(out_dir, "names.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    if assets:
        with open(os.path.join(out_dir, "PICTURES.ZIF"), "wb") as f:
            f.write(serialize([Chunk(b"ULIB", kids=assets)]))


# ---- the attract demo's replays ---------------------------------------------------------

# REPLAY.ZAL (a zfile library, uncompressed: a header, counts at +0x10, then
# a directory of groups {u32 id, {u32 offset from byte 0x10, u32 length}}
# and the assets): groups 0-15 are the attract demo's, two a level (group
# level * 2 + choice: levelStateEnter), 16 on the NPCs'. A demo replay's
# saved part starts with the controller layout, the multiplayer mode, the
# single-player mode and the level; the rider record's first bytes from +0x0C
# (its name first, "Rider00").
DEMO_GROUPS = 16
ZFILE_MAGIC = 0x2A81511C


def library_groups(data):
    """[(id, offset, length)] of an uncompressed one-asset-a-group zfile library."""
    if struct.unpack_from("<I", data, 0)[0] != ZFILE_MAGIC:
        raise ValueError("REPLAY.ZAL is not a zfile library")
    assets, groups = struct.unpack_from("<HH", data, 0x10)
    if (assets & 0x1FFF) != 1 or assets >> 15 or groups >> 14:
        raise ValueError("REPLAY.ZAL is not an uncompressed one-asset-a-group library")
    out = []
    for g in range(groups & 0x3FFF):
        gid, off, length = struct.unpack_from("<III", data, 0x14 + 12 * g)
        out.append((gid, off, length & 0xFFFFFF))
    return out


def demo_replay_ok(data, offset, length, level):
    body = data[0x10 + offset:0x10 + offset + length]
    return len(body) > 0x20 and body[1] == 0 and body[3] == level and body[0x0C:0x11] == b"Rider"


def demo_replays(xbox, ps2, out_dir):
    """demo/REPLAY.ZAL: the PS2's with each demo group the PS2's holds no replay of its level for (its disc keeps
    two, level 3's; the rest are left-over bytes that crash the level they load) taken from the Xbox's, which holds
    the demos its attract mode plays (levels 3, 2 and 7: its table 0x1A0D10). The NPCs' groups stay the PS2's."""
    ps2_data, xbox_data = ps2.read("REPLAY.ZAL"), xbox.read("replay.zal")
    if ps2_data is None or xbox_data is None:
        raise ValueError("no REPLAY.ZAL in the PS2 or the Xbox files")
    ours, theirs = library_groups(ps2_data), library_groups(xbox_data)
    out = bytearray(ps2_data)
    taken, kept = [], []
    for g in range(DEMO_GROUPS):
        level = g // 2
        if demo_replay_ok(ps2_data, ours[g][1], ours[g][2], level):
            kept.append(g)
            continue
        if g < len(theirs) and theirs[g][0] == g and demo_replay_ok(xbox_data, theirs[g][1], theirs[g][2], level):
            body = xbox_data[0x10 + theirs[g][1]:0x10 + theirs[g][1] + theirs[g][2]]
            out += b"\0" * (-len(out) % 16)
            struct.pack_into("<II", out, 0x14 + 12 * g + 4, len(out) - 0x10, len(body))
            out += body
            taken.append(g)
    os.makedirs(os.path.join(out_dir, "demo"), exist_ok=True)
    with open(os.path.join(out_dir, "demo", "REPLAY.ZAL"), "wb") as f:
        f.write(bytes(out))
    say("attract demo: demo/REPLAY.ZAL, the PS2's demo groups %s kept, the Xbox's %s taken" % (kept, taken))


# ---- the Retro movie ----------------------------------------------------------------

# The PS2's movies: MPEG-2 video in stream 0xE0 at the movie's own rate, the
# sound in private stream 1 (0xBD, after a 4-byte sub-stream header, ff a0 00 00
# as the PS2 disc's) as "SShd" (size 0x18: type 1 = 16-bit PCM, rate, channels,
# interleave) and "SSbd" (size) then the samples, each channel's in turn in
# blocks of the interleave's bytes (DAVE.PSS: 48 kHz, stereo, 512).
PSS_RATE, PSS_CHANNELS, PSS_INTERLEAVE = 48000, 2, 512
PSS_PACKET = 2048
PSS_SUBSTREAM = b"\xff\xa0\x00\x00"
RETRO_BITRATE = "5000k"  # INTRO.PSS: 104 s, 512x256, 86.7 MB with its sound


def pss_pack_header(scr):
    """An MPEG-2 pack header (14 bytes) at system clock `scr` (90 kHz), mux rate 25200 (10.08 Mbit/s)."""
    b = bytearray(14)
    b[0:4] = b"\x00\x00\x01\xba"
    b[4] = 0x44 | ((scr >> 27) & 0x38) | ((scr >> 28) & 0x03)
    b[5] = (scr >> 20) & 0xFF
    b[6] = ((scr >> 12) & 0xF8) | 0x04 | ((scr >> 13) & 0x03)
    b[7] = (scr >> 5) & 0xFF
    b[8] = ((scr << 3) & 0xF8) | 0x04
    b[9] = 0x01
    mux = 25200
    b[10], b[11], b[12] = (mux >> 14) & 0xFF, (mux >> 6) & 0xFF, ((mux << 2) & 0xFC) | 0x03
    b[13] = 0xF8
    return bytes(b)


def pss_pts(pts):
    return bytes(((0x21 | ((pts >> 29) & 0x0E)), (pts >> 22) & 0xFF, ((pts >> 14) & 0xFE) | 1, (pts >> 7) & 0xFF,
                  ((pts << 1) & 0xFE) | 1))


def pss_mux(video, sound, fps):
    """A PSS of an MPEG-2 video elementary stream and 16-bit stereo PCM (interleaved L R samples): one PES per
    2048-byte packet, the two streams interleaved by their time, each PES with its time (PTS)."""
    import array
    samples = array.array("h", sound[:len(sound) // 4 * 4])
    if sys.byteorder != "little":
        samples.byteswap()
    left, right = samples[0::2], samples[1::2]
    per = PSS_INTERLEAVE // 2
    pad = (-len(left)) % per
    left.extend([0] * pad)
    right.extend([0] * pad)
    if sys.byteorder != "little":
        left.byteswap()
        right.byteswap()
    pcm = bytearray()
    for at in range(0, len(left), per):
        pcm += left[at:at + per].tobytes() + right[at:at + per].tobytes()
    audio =b"SShd" + struct.pack("<5I", 0x18, 1, PSS_RATE, PSS_CHANNELS, PSS_INTERLEAVE) + b"\xff" * 8 + \
        b"SSbd" + struct.pack("<I", len(pcm)) + bytes(pcm)
    seconds = max(len(pcm) / (PSS_RATE * 2 * PSS_CHANNELS), 1e-6)
    out = bytearray()
    v_at = a_at = 0
    # PES payload room: packet - pack header - PES start (6) - flags (3) - PTS (5) [- substream header].
    v_room = PSS_PACKET - 14 - 6 - 3 - 5
    a_room = v_room - len(PSS_SUBSTREAM)
    while v_at < len(video) or a_at < len(audio):
        v_time = v_at / max(len(video), 1) * seconds
        a_time = max(a_at - 0x28, 0) / (PSS_RATE * 2 * PSS_CHANNELS)
        is_video = a_at >= len(audio) or (v_at < len(video) and v_time <= a_time)
        t = v_time if is_video else a_time
        pts = int(t * 90000) + 9000
        out += pss_pack_header(max(pts - 9000, 0))
        if is_video:
            chunk = video[v_at:v_at + v_room]
            v_at += len(chunk)
            body = b"\x81\x80\x05" + pss_pts(pts) + chunk
            sid = 0xE0
        else:
            chunk = audio[a_at:a_at + a_room]
            a_at += len(chunk)
            body = b"\x81\x80\x05" + pss_pts(pts) + PSS_SUBSTREAM + chunk
            sid = 0xBD
        out += b"\x00\x00\x01" + bytes([sid]) + struct.pack(">H", len(body)) + body
    out += b"\x00\x00\x01\xb9"
    return bytes(out)


def retro_movie(xbox, out_dir, ffmpeg):
    """movies/ASSETS/MOVIE/RETRO.PSS: the Xbox's movie/retro.bik (512x256, 30 frames a second, 48 kHz stereo) as
    the PS2 gallery's 16th movie (g_menuMovies[15], assets\\movie\\retro.pss): the picture as MPEG-2 at its own size
    and rate, the sound as the PS2 movies' PCM."""
    import subprocess
    import tempfile
    source = XboxFiles.find(xbox.root, "movie/retro.bik")
    if not source:
        raise ValueError("movie/retro.bik is not in the Xbox files")
    target = os.path.join(out_dir, "ASSETS", "MOVIE", "RETRO.PSS")
    os.makedirs(os.path.dirname(target), exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        m2v, raw = os.path.join(tmp, "retro.m2v"), os.path.join(tmp, "retro.pcm")
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-an", "-pix_fmt", "yuv420p",
                        "-c:v", "mpeg2video", "-b:v", RETRO_BITRATE, "-maxrate", "8000k", "-bufsize", "1835008",
                        "-g", "15", "-bf", "2", "-aspect", "4:3", "-f", "mpeg2video", m2v], check=True)
        subprocess.run([ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", source, "-vn", "-ac", str(PSS_CHANNELS),
                        "-ar", str(PSS_RATE), "-f", "s16le", raw], check=True)
        with open(m2v, "rb") as f:
            video = f.read()
        with open(raw, "rb") as f:
            sound = f.read()
    data = pss_mux(video, sound, 30.0)
    with open(target + ".tmp", "wb") as f:
        f.write(data)
    os.replace(target + ".tmp", target)
    say("Retro movie: movies/ASSETS/MOVIE/RETRO.PSS, %.1f s of sound, %.1f MB" % (
        len(sound) / (PSS_RATE * 2 * PSS_CHANNELS), len(data) / 1e6))


# ---- the level select ----------------------------------------------------------

def level_texts(xbox):
    """{level: info text} from the Xbox string table (KSTR_LEVELINFOMENU_<v>)."""
    data = xbox.read("string.eus")
    out = {}
    if data is None:
        return out
    count = struct.unpack_from("<I", data, 4)[0]
    table = {}
    for i in range(count):
        key, at = struct.unpack_from("<II", data, 8 + 8 * i)
        table[key] = data[at:data.index(b"\0", at)]
    for level, value in XBOX_VALUES.items():
        text = table.get(name_hash("KSTR_LEVELINFOMENU_%d" % value))
        if text:
            out[level] = text
    return out


UI_CONTAINERS = {b"ULIB", b"UAST", b"UMTL", b"TXRC"}


def parse_ui(data, start=0, end=None):
    """A UI library's chunk tree (ULIB > UAST > HAST, UMTL > HMTL, TXRC)."""
    end = len(data) if end is None else end
    out, at = [], start
    while at + 8 <= end:
        tag, size = data[at:at + 4], struct.unpack_from("<I", data, at + 4)[0]
        body = at + 8
        if body + size > end:
            raise ValueError("chunk %r runs past its parent" % tag)
        out.append(Chunk(tag, kids=parse_ui(data, body, body + size)) if tag in UI_CONTAINERS
                   else Chunk(tag, data[body:body + size]))
        at = body + size
    return out


def picture_assets(xbox, levels):
    """The Xbox UILIB's level pictures (LEVELS.ZIF's UAST "level<v>": HAST
    {path[128], name[32], u32}, UMTL {HMTL, TXRC {TXRH, TBMP 256x256 8-bit}},
    the PS2's format), the asset and its texture named "xb_<level>" for the
    runtime (which names them "Level<menu value>")."""
    uilib = xbox.read("uilib.zal")
    if uilib is None:
        return []
    want = {"level%d" % XBOX_VALUES[l]: l for l in levels}
    found = {}
    for entry in dm2_level.zal_entries(uilib):
        if entry[:4] != b"ULIB":
            continue
        try:
            lib = parse_ui(entry)
        except ValueError:
            continue
        for asset in (a for c in lib if c.tag == b"ULIB" for a in c.kids):
            if asset.tag != b"UAST" or not asset.kids or asset.kids[0].tag != b"HAST":
                continue
            hast = asset.kids[0].data
            name = hast[0x80:0xA0].split(b"\0")[0].decode("latin1").lower()
            if name not in want or want[name] in found:
                continue
            level = want[name]
            new = ("xb_" + level.lower()).encode("latin1")
            asset.kids[0].data = hast[:0x80] + new.ljust(32, b"\0") + hast[0xA0:]
            for c in walk(asset.kids):
                if c.tag == b"TXRH":
                    c.data = struct.pack("<I", name_hash(new.decode())) + c.data[4:0x14] + new.ljust(32, b"\0") + \
                        c.data[0x34:]
            found[level] = asset
    return [found[l] for l in levels if l in found]


# ---- commands -------------------------------------------------------------------

def convert_level(xbox, level, out_dir, max_side, jobs, budget=0):
    from collections import Counter
    report = Counter()
    folder = LEVELS[level][0]
    files = {}
    for ext in ("ZIT", "GEO", "INS", "TXR", "DAT"):
        rel = "levels/%s/%s.%s" % (folder, folder, ext)
        data = xbox.read(rel)
        if data is None:
            raise ValueError("%s is not in the Xbox files" % rel)
        files[ext] = data
    # Textures of materials with an Xbox material type (MATI flags 0x4,
    # 0x8: grass) are drawn opaque, as the PS2's grass base layers are.
    grass = {t for t, flags in material_textures(files["GEO"]).items() if any(f & 0x3FC for f in flags)}
    txr, sizes, entries = convert_txr(files["TXR"], max_side, jobs, report, grass, budget)
    offsets = {}
    made = {
        "ZIT": convert_zit(files["ZIT"], level),
        "GEO": convert_geo(files["GEO"], sizes, report, offsets),
        "INS": convert_ins(files["INS"], report, offsets),
        "TXR": txr,
        "DAT": convert_dat(files["DAT"], report),
        "XTP": write_pack(entries),
    }
    os.makedirs(out_dir, exist_ok=True)
    for ext, data in made.items():
        tmp = os.path.join(out_dir, "%s.%s.tmp" % (level, ext))
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join(out_dir, "%s.%s" % (level, ext)))
    start = report.pop("start")
    say("%s: %d objects, %d parts, %d faces, %d materials, %d textures (%d made smaller, %d keyed), strip groups "
        "%d -> %d, start (%.1f, %.1f, %.1f); GEO %.1f -> %.1f MB, TXR %.1f -> %.1f MB, XTP %.1f MB; %s" % (
            level, report["objects"], report["parts"], report["faces"], report["materials"], report["textures"],
            report["textures smaller"], report["textures keyed (cut out)"], report["strip groups in"],
            report["strip groups out"], start[0], start[1], start[2], len(files["GEO"]) / 1e6, len(made["GEO"]) / 1e6,
            len(files["TXR"]) / 1e6, len(txr) / 1e6, len(made["XTP"]) / 1e6,
            ", ".join("%s %d" % (k, v) for k, v in sorted(report.items())
                      if k.startswith(("dropped", "unused", "second", "type", "objects recentred", "instances moved", "material flags", "clamp", "spawns",
                                       "textures ARGB", "textures DXT", "textures smaller for", "texel bytes")))))
    return report


def cmd_convert(args):
    global KEEP_LODS
    KEEP_LODS = args.keep_lods
    xbox = XboxFiles(args.xbox)
    only = set(args.only.split(",")) if args.only else {"levels", "themes", "demo", "movies"}
    if "themes" in only:
        # The park editor's Desert and Highway: with the PS2's files (Beach's
        # THEME block and light points).
        if args.ps2:
            convert_themes(xbox, Ps2Files(args.ps2), os.path.join(args.out, "themes"), not args.no_pictures,
                           args.picture_dir)
        else:
            say("themes: no --ps2 (the PS2 disc's files): no Desert and Highway")
    if "demo" in only and args.ps2:
        demo_replays(xbox, Ps2Files(args.ps2), args.out)
    if "movies" in only:
        ffmpeg = args.ffmpeg or shutil.which("ffmpeg")
        if ffmpeg:
            retro_movie(xbox, os.path.join(args.out, "movies"), ffmpeg)
        else:
            say("movies: no ffmpeg: no Retro movie")
    if "levels" not in only:
        return
    levels = [l.upper() for l in (args.level or LEVELS)]
    for level in levels:
        if level not in LEVELS:
            raise ValueError("%s is not one of %s" % (level, ", ".join(LEVELS)))
    jobs = args.jobs or min(8, os.cpu_count() or 1)
    texts = level_texts(xbox)
    lines = []
    for level in levels:
        out_dir = os.path.join(args.out, level)
        convert_level(xbox, level, out_dir, args.max_side, jobs, args.texture_budget * 1024)
        if level in texts:
            with open(os.path.join(out_dir, "INFO.TXT"), "wb") as f:
                f.write(texts[level])
        lines.append("%s %s" % (level, LEVELS[level][1]))
    assets = picture_assets(xbox, levels)
    if assets:
        data = serialize([Chunk(b"ULIB", kids=assets)])
        with open(os.path.join(args.out, "PICTURES.ZIF"), "wb") as f:
            f.write(data)
        say("level pictures: %d in PICTURES.ZIF (%d bytes)" % (len(assets), len(data)))
    with open(os.path.join(args.out, "levels.txt"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    # The career: goals, gaps, rewards, local pros and texts of both levels,
    # from the executable (dm2_xbox_career.py).
    career, report, _ = dm2_xbox_career.build(xbox)
    with open(os.path.join(args.out, "CAREER.BIN"), "wb") as f:
        f.write(career)
    say("career: %d goals (%d parts), Venice %d gaps, Greenville %d gaps, %d extra rewards, %d texts; CAREER.BIN %d bytes"
        % (report["goals"], report["parts"], report["gaps VENICE"], report["gaps GREENVIL"], report["extra rewards"],
           report["texts"], len(career)))


def cmd_info(args):
    dm2_level.cmd_info(args)


def read_out(args):
    base = os.path.join(args.dir, args.level.upper(), args.level.upper())
    with open(base + ".GEO", "rb") as f:
        geo = f.read()
    with open(base + ".INS", "rb") as f:
        ins = f.read()
    return geo, ins


def cmd_ground(args):
    """The converted level's own collidable surface under a grid (dm2_level.py's
    reference: the faces its collision trees list, seen from above, of the
    instances that collide): <points> for the runtime's ZX_DM2_XBOX_LEVEL_PROBE,
    <out> "x z y|none" per point."""
    geo, ins = read_out(args)
    groups = dm2_level.collision_triangles(geo, ins)
    x0, x1 = min(g[0] for g in groups), max(g[1] for g in groups)
    z0, z1 = min(g[2] for g in groups), max(g[3] for g in groups)
    points, out = [], []
    x = x0 + args.step / 2 + 0.0137
    while x < x1:
        z = z0 + args.step / 2 + 0.0171
        while z < z1:
            points.append("%.3f %.3f\n" % (x, z))
            y = dm2_level.topmost(groups, x, z)
            out.append("%.3f %.3f %s\n" % (x, z, "none" if y is None else "%.4f" % y))
            z += args.step
        x += args.step
    with open(args.points, "w", newline="\n") as f:
        f.writelines(points)
    with open(args.out, "w", newline="\n") as f:
        f.writelines(out)
    say("%s: %d points every %g over x %.0f..%.0f, z %.0f..%.0f" % (args.level.upper(), len(points), args.step,
                                                                 x0, x1, z0, z1))


def grind_edges(geo, ins):
    """Every grindable edge of the colliding instances in world space,
    [(a, b, name, kind)]: DM2's TGFE records {u32 hash, kind, flags, 3,
    material}, grindable when flags bit 0 (FUN_001ACEB8); edge e = face *
    3 + k runs from the face's vertex k to k + 1 (FUN_0017A080)."""
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
            desc, count, indices, records = dm2_level.tag_buffer(streams[b"TGFE"].data)
            edges = []
            for e, r in enumerate(struct.unpack_from("<%dH" % count, indices)):
                if r == 0xFFFF or not records[r][5] & 1 or e // 3 >= nf:
                    continue
                face = faces[e // 3]
                i0, i1 = face[1 + e % 3], face[1 + (e + 1) % 3]
                if max(i0, i1) < nv:
                    edges.append((verts[i0], verts[i1], records[r][4]))
            objects[struct.unpack_from("<I", head.data, 0x18)[0]] = edges
    instances = [c.data for c in walk(parse(ins)) if c.tag == b"TGIH"]
    by_id = {struct.unpack_from("<I", d, 0x48)[0]: d for d in instances}

    def place(d, v):
        for _ in range(16):
            v = tuple(a + b for a, b in zip(dm2_level.quat_rotate(struct.unpack_from("<4f", d, 8), v),
                                            struct.unpack_from("<3f", d, 0x18)))
            parent = struct.unpack_from("<I", d, 0x44)[0]
            d = by_id.get(parent) if parent else None
            if d is None:
                break
        return v
    out = []
    for d in instances:
        if struct.unpack_from("<I", d, 0)[0] & 0x12 != 0x2:
            continue
        name = d[0x4C:].split(b"\0")[0].decode("latin1")
        for a, b, kind in objects.get(struct.unpack_from("<I", d, 0x40)[0], ()):
            out.append((place(d, a), place(d, b), name, kind))
    return out


def cmd_edges(args):
    geo, ins = read_out(args)
    seen, lines = set(), []
    for a, b, name, kind in grind_edges(geo, ins):
        key = tuple(sorted((tuple(round(v, 3) for v in a), tuple(round(v, 3) for v in b))))
        if key in seen:
            continue
        seen.add(key)
        lines.append("%.4f %.4f %.4f %.4f %.4f %.4f %d %s\n" % (a + b + (kind, name)))
    with open(args.out, "w", newline="\n") as f:
        f.writelines(lines)
    say("%s: %d grindable edges" % (args.level.upper(), len(lines)))


def cmd_compare(args):
    """The level's own ground (cmd_ground's out) against DM2's
    (ZX_DM2_XBOX_LEVEL_PROBE's out), point by point."""
    from collections import Counter
    counts = Counter()
    with open(args.level_ground) as a, open(args.dm2) as b:
        for ref, got in zip(a, b):
            ref, got = ref.split(), got.split()
            if ref[2] == "none" or got[2] == "none":
                counts["level %s, DM2 %s" % ("none" if ref[2] == "none" else "ground", "none" if got[2] == "none" else "ground")] += 1
                continue
            d = abs(float(got[2]) - float(ref[2]))
            counts["same height (0.01)" if d <= 0.01 else "within 0.1" if d <= 0.1 else "different"] += 1
    total = sum(counts.values())
    for key, n in sorted(counts.items()):
        print("%-24s %6d  %6.2f%%" % (key, n, 100.0 * n / total))
    ground = total - counts["level none, DM2 none"]
    same = counts["same height (0.01)"] + counts["within 0.1"]
    if ground:
        print("ground in either: %d points, DM2's within 0.1 of the level's: %d (%.1f%%)" % (ground, same, 100.0 * same / ground))


def cmd_selftest(args):
    """The conversions on hand-made records (no disc needed)."""
    from collections import Counter
    # Strips: two one-strip groups of submesh 0 and one of submesh 1, packed.
    groups = [(0, [0x8000, 0x8000, 1, 2, 3]), (0, [0x8004, 0x8005, 6]), (0x100, [0x8007, 0x8008, 9, 10])]
    packed = pack_groups(groups)
    assert packed == [(0, [0x8000, 0x8000, 1, 2, 3, 0x8004, 0x8005, 6]), (0x100, [0x8007, 0x8008, 9, 10])], packed
    assert triangles(packed) == triangles(groups) == [(0, 1, 2), (1, 2, 3), (4, 5, 6), (7, 8, 9), (8, 9, 10)]
    assert read_groups(write_groups(packed)) == packed
    long = [(0, [0x8000 + 3 * k, 0x8001 + 3 * k, 2 + 3 * k]) for k in range(25)]
    assert [len(g[1]) for g in pack_groups(long)] == [60, 15]
    # A part in the later exporter's order comes out in the PS2's, its group count updated.
    head = struct.pack("<IHHIHHI", 0, 11, 5, 3, 0, 2, 7)
    part = Chunk(b"TGPT", kids=[Chunk(b"TGPH", head)] + [Chunk(t, b"") for t in (b"TGVC", b"TGVN", b"TGVU", b"TGFN",
                                                                                  b"TGVP", b"TGFP")]
                 + [Chunk(b"TGFM", write_groups(groups))])
    report = Counter()
    convert_part(part, report)
    assert [k.tag for k in part.kids] == list(PS2_PART_ORDER[:8]), [k.tag for k in part.kids]
    assert struct.unpack_from("<I", part.kids[0].data, 8)[0] == 2 and report["strip groups out"] == 2
    # Materials: flags 0x6 -> 0x2; a 256 texture's region repeat (mask 255) at 128.
    mati = bytearray(dm2_level.DM2_MATI)
    struct.pack_into("<I", mati, 0, 6)
    struct.pack_into("<I", mati, 0x28, 1)
    w0 = 0x1C | (3 << 10) | (3 << 12) | (255 << 14)
    w1 = 0 | (255 << 10) | (0 << 20)
    struct.pack_into("<2I", mati, 0x2C, w0, w1)
    struct.pack_into("<I", mati, 0x2C + 0x10, 77)
    out = convert_mati(bytes(mati), {77: ((256, 256), (128, 128))}, Counter())
    o0, o1 = struct.unpack_from("<2I", out, 0x2C)
    assert struct.unpack_from("<I", out, 0)[0] == 2
    assert (o0 >> 14) & 0x3FF == 127 and (o1 >> 10) & 0x3FF == 127 and o1 & 0x3FF == 0 and o0 & 0x3FFF == w0 & 0x3FFF
    assert scale_clamp(0x1C | (3 << 10) | (63 << 14), 64, 0.5, 0.5)[1] & 0x3FF == 32  # fix halves
    # Textures: a 4x4 DXT3 block, half clear (a cut-out: keyed), and a DXT1 one.
    alpha = sum((0 if i < 8 else 15) << (4 * i) for i in range(16))
    block = alpha.to_bytes(8, "little") + struct.pack("<HHI", 0xF800, 0x001F, 0x55555555)
    rows = dxt_decode(block, 4, 4, b"DXT3")
    assert rows[0][0][3] == 0 and rows[3][3] == (0, 0, 255, 255) and rows[0][0][:3] == (0, 0, 255)
    dds = bytearray(128)
    dds[0:4] = b"DDS "
    struct.pack_into("<II", dds, 12, 4, 4)
    struct.pack_into("<I", dds, 28, 1)
    dds[84:88] = b"DXT3"
    txrh = struct.pack("<IHHI", 77, 4, 4, 0) + b"\0" * 8 + b"T".ljust(32, b"\0")
    txrc, r, entry = convert_texture((txrh, bytes(dds) + block, 128, False))
    assert not convert_texture((txrh, bytes(dds) + block, 128, True))[1]["keyed"]
    assert r["keyed"] and r["cutout"] and r["ps2"] == (4, 4) and entry[3][3] == 1
    tex = parse(txrc)[0]
    w, h, prow = dm2_level.decode_bmp(tex.kids[1].data)
    assert (w, h) == (4, 4) and prow[0][0][:3] == (255, 0, 255) and prow[3][3][:3] != (255, 0, 255)
    assert write_pack([entry])[:4] == b"PXTP"
    d1 = struct.pack("<HHI", 0xFFFF, 0x0000, 0)
    assert dxt_decode(d1, 4, 4, b"DXT1")[0][0] == (255, 255, 255, 255)
    # Halving keeps clear texels from darkening their neighbours.
    assert halve([[(200, 0, 0, 255), (0, 0, 0, 0)], [(200, 0, 0, 255), (0, 0, 0, 0)]]) == [[(200, 0, 0, 128)]]
    # Binned colours never read as the key.
    assert not is_key_colour(bin6((255, 0, 255, 255)))
    # The texture budget: the largest textures one level smaller first, the
    # DDS's own mips (a 256 with mips at 128 is 128+64+32+16 squared).
    class FakeDds:
        def __init__(self, side, mips):
            self.mips = mips
            self.offsets = [(0, 0, max(1, side >> k), max(1, side >> k)) for k in range(mips)]
    big, small = FakeDds(256, 9), FakeDds(64, 7)
    assert ps2_texels(big, 128) == 128 * 128 + 64 * 64 + 32 * 32 + 16 * 16 and ps2_texels(small, 128) == 64 * 64 + 32 * 32 + 16 * 16 + 8 * 8
    assert budget_sides([big, small], 128, 10 ** 9) == ([128, 128], ps2_texels(big, 128) + ps2_texels(small, 128))
    sides, total = budget_sides([big, small, FakeDds(256, 9)], 128, 2 * ps2_texels(big, 64) + ps2_texels(small, 128))
    assert sides == [64, 128, 64] and total == 2 * ps2_texels(big, 64) + ps2_texels(small, 128)
    # Themes: the donor's block with our NAME and FILE; instances raised.
    block = theme_block('THEME {\n\tNAME\t"Beach"\n\tFILE\t"LEBeach"\n\tAMBIANT 1 2 3\n}', "Desert", "LEDESERT")
    assert '"Desert"' in block and '"LEDESERT"' in block and "AMBIANT 1 2 3" in block and "Beach" not in block
    assert [f for f, _ in theme_blocks("THEME {\n FILE \"A\"\n}\nTHEME {\n FILE \"B\"\n}\nEND")] == ["A", "B"]
    ins = serialize([Chunk(b"FORM", kids=[Chunk(b"TGIH", struct.pack("<II4f3f", 0x23, 0, 0, 0, 0, -1, 1.0, 2.0, 3.0) + b"\0" * 72)])])
    assert struct.unpack_from("<3f", parse(raise_ins(ins, -15.0, Counter()))[0].kids[0].data, 0x18) == (1.0, -13.0, 3.0)
    # The PSS: each stream's payloads in order make its input again; the sound as the PS2's PCM blocks.
    video = bytes(range(256)) * 40
    sound = struct.pack("<%dh" % 1200, *([1000, -1000] * 600))
    pss = pss_mux(video, sound, 30.0)
    streams, at = {0xE0: b"", 0xBD: b""}, 0
    while True:
        at = pss.find(b"\x00\x00\x01", at)
        if at < 0 or pss[at + 3] == 0xB9:
            break
        if pss[at + 3] in streams:
            size = struct.unpack_from(">H", pss, at + 4)[0]
            body = pss[at + 6:at + 6 + size]
            skip = 3 + body[2] + (len(PSS_SUBSTREAM) if pss[at + 3] == 0xBD else 0)
            streams[pss[at + 3]] += body[skip:]
            at += 6 + size
        else:
            at += 4
    assert streams[0xE0] == video
    a = streams[0xBD]
    assert a[:4] == b"SShd" and struct.unpack_from("<5I", a, 4) == (0x18, 1, PSS_RATE, PSS_CHANNELS, PSS_INTERLEAVE)
    assert a[0x20:0x24] == b"SSbd" and struct.unpack_from("<I", a, 0x24)[0] == 2 * PSS_INTERLEAVE * 3
    assert struct.unpack_from("<2h", a, 0x28) == (1000, 1000) and struct.unpack_from("<h", a, 0x28 + PSS_INTERLEAVE)[0] == -1000
    dm2_xbox_career.cmd_selftest(args)
    print("dm2_xbox.py selftest: ok")


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("convert")
    c.add_argument("--xbox", required=True, help="the DM2 Xbox disc's files (tools/xbox_files.py extract)")
    c.add_argument("--out", required=True)
    c.add_argument("--level", action="append")
    c.add_argument("--max-side", type=int, default=MAX_SIDE)
    c.add_argument("--jobs", type=int, default=0, help="worker processes for the textures (0: up to 8)")
    c.add_argument("--ps2", help="the DM2 PS2 disc's files (its root or ASSETS): the park editor themes")
    c.add_argument("--ffmpeg", help="ffmpeg for the Retro movie (default: the one on PATH)")
    c.add_argument("--only", help="levels,themes,demo,movies (default: all)")
    c.add_argument("--no-pictures", action="store_true", help="developers: the themes without their pictures")
    c.add_argument("--picture-dir", help="developers: each theme's picture as a PNG here too")
    c.add_argument("--keep-lods", action="store_true", help="developers: keep the props' second levels of detail")
    c.add_argument("--texture-budget", type=int, default=TEXTURE_BUDGET_KB, metavar="KB",
                   help="the texels of a level's textures (mips included) at most this many KB, the largest made "
                        "smaller first (default %d; 0: none, developers). DM2's texture "
                        "cache needs it: a frame's textures must fit their part of GS memory, which the cache does "
                        "not check" % TEXTURE_BUDGET_KB)
    c.set_defaults(fn=cmd_convert)
    i = sub.add_parser("info")
    i.add_argument("files", nargs="+")
    i.set_defaults(fn=cmd_info)
    g = sub.add_parser("ground")
    g.add_argument("--dir", required=True)
    g.add_argument("--level", required=True)
    g.add_argument("--step", type=float, default=5.0)
    g.add_argument("--points", required=True)
    g.add_argument("--out", required=True)
    g.set_defaults(fn=cmd_ground)
    e = sub.add_parser("edges")
    e.add_argument("--dir", required=True)
    e.add_argument("--level", required=True)
    e.add_argument("--out", required=True)
    e.set_defaults(fn=cmd_edges)
    m = sub.add_parser("compare")
    m.add_argument("level_ground")
    m.add_argument("dm2")
    m.set_defaults(fn=cmd_compare)
    t = sub.add_parser("selftest")
    t.set_defaults(fn=cmd_selftest)
    args = p.parse_args(argv)
    try:
        args.fn(args)
    except ValueError as e:
        raise SystemExit("dm2_xbox.py: %s" % e)


if __name__ == "__main__":
    main(sys.argv[1:])
