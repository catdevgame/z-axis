#!/usr/bin/env python3
"""Xbox texture pack for the native port: the Xbox release's textures for the
PS2 ones they replace, made at build time from your own two discs.

    python3 -I tools/xbox_pack.py --ps2 <PS2 disc folder> --xbox <Xbox folder> --out <pack folder>

--ps2: the extracted PS2 disc (the folder with SYSTEM.CNF and ASSETS, or ASSETS).
--xbox: the extracted Xbox disc (its root with default.xbe, its media folder, or
media/assets). Both are read only. Python 3.8+, standard library only (runs on
the Windows build machine too).

Output (the app bundles it as Resources/xbox-pack; never commit it, it is made
from the discs):
    <out>/textures/<PS2 path>.xtp   one per PS2 file with replacements, e.g.
                                    textures/ASSETS/LEVELS/CIVIC/CIVIC.TXR.xtp
    <out>/textures/report.txt       coverage per file, rejected pairs
    <out>/movies/<PS2 path>.bik     the Xbox Bink movie for each PS2 movie, e.g.
                                    movies/ASSETS/MOVIES/CUTSCENE/CIVIC.PSS.bik
                                    (copied as is; --no-movies leaves them out)
    <out>/.stamp                    inputs' sizes and times (reuse check)

Both releases use Z-Axis' IFF texture libraries: TXRC chunks with a TXRH
header (u32 id, u16 w, u16 h, 12 zero bytes, name[32] at +0x14) and the
picture: PS2 TBMP/TZMP (a Windows BMP, 4 or 8 bit), Xbox TXPR (u32 0 then an
XPR0 bundle with one D3D texture: DXT3 with a full mip chain for levels,
skaters and editor pieces). The Xbox files keep the PS2 names (sizes dropped
from skater names, IDn slots kept); a few renamed ones are paired by hand
below. A pair goes into the pack if the Xbox picture is DXT3 and at least as
large (the Xbox release's pictures are the default in the port wherever the
Xbox has a counterpart: redrawn, sharper, or its own DXT3 of the same size),
and if both look alike (8x8 thumbnails: mean RGB difference where both are
opaque under --max-diff, and the same transparent parts within --max-cover;
the PS2 marks those magenta, the Xbox with DXT3 alpha), so a wrong name
match never shows. The colour limit is loose on purpose: some Xbox pictures
were redrawn (a real flag, a grate), drawn at the same place by the Xbox
game. Xbox pictures with opaque magenta texels (the PS2 colour key left in,
pink fringes) are left out.

The .xtp format is ps2x::texture_replace::parsePack's (PS2Recomp
runtime/include/runtime/ps2_texture_replace.h). The key is what the GS
holds once the game has uploaded the texture: its size and the FNV-1a 64 of
its indices, one byte per texel, rows top down (the BMP's bottom-up rows
reversed, as the game loads them), 4-bit indices unpacked (first texel in
the high nibble of a BMP byte).
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import struct
import sys
import time

FORMAT_BC2 = 1
XBOX_DXT3 = 0x0E

# Renamed textures (PS2 file, PS2 name) -> Xbox name, all lower case; found
# by picture (compare_textures.py in the Xbox survey) and checked by eye.
RENAMED = {
    ("levels/hanger/hanger.txr", "grassa01"): "o_grassa01",
    ("skaters/sboek/sb_zit.zit", "svenmid"): "svenmid_512",
    ("skaters/statue/st_zit.zit", "statue_256_id1_v1"): "statfacea01",
    ("skaters/statue/st_zit.zit", "statue_256_id2_v1"): "statmida01",
    ("skaters/statue/st_zit.zit", "statue_256_id3_v1"): "statbottoma01",
    ("skaters/tinman/tin_zit.zit", "tinfacea01"): "tinman_512_id1_v1",
    ("skaters/tinman/tin_zit.zit", "tinmida01"): "tinman_512_id2_v1",
    ("skaters/tinman/tin_zit.zit", "tinbottoma01"): "tinman_512_id3_v1",
}
# Xbox-only crops of PS2 textures drawn with REGION_CLAMP: never a pair.
CROP = re.compile(r"_[0-9a-f]{6,8}$", re.I)


# ---- IFF texture libraries ---------------------------------------------------


class Tex:
    __slots__ = ("name", "kind", "data", "w", "h", "bpp", "fmt", "mips", "pixels", "levels")

    def __init__(self, name, kind, data):
        self.name, self.kind, self.data = name, kind, data
        self.w = self.h = self.bpp = self.fmt = self.mips = 0
        self.pixels = 0  # Xbox: offset of mip 0 in data
        self.levels = []  # PS2: the smaller pictures after the first (mip 1 ...), as Tex


def chunks(d, p, end):
    while p + 8 <= end:
        tag = d[p:p + 4]
        size = struct.unpack_from("<I", d, p + 4)[0]
        yield tag, p + 8, size
        p += 8 + size


def read_txrc(d, b, size):
    """A TXRC: the TXRH, then the picture; a PS2 picture may be followed by
    its smaller mip levels (TBMP chunks of half the size, for GS mipmaps)."""
    name, pics = "", []
    for tag, cb, cs in chunks(d, b, b + size):
        if tag == b"TXRH":
            name = d[cb + 0x14:cb + cs].split(b"\0")[0].decode("latin1")
        elif tag in (b"TBMP", b"TZMP", b"TXPR", b"TTGA"):
            pics.append(Tex(name, tag.decode(), d[cb:cb + cs]))
    if not pics:
        return None
    for t in pics:
        t.name = name
        if t.kind in ("TBMP", "TZMP") and len(t.data) >= 54:
            _, _, _, _, _, _, w, h, _, bpp = struct.unpack_from("<2sIHHIIiiHH", t.data, 0)
            t.w, t.h, t.bpp = w, h, bpp  # h < 0: top-down rows
        elif t.kind == "TXPR":
            xpr_header(t)
    base = pics[0]
    base.levels = [t for t in pics[1:] if t.kind == base.kind and t.bpp == base.bpp]
    return base


def iter_txrc(d, p=0, end=None):
    end = len(d) if end is None else end
    for tag, b, sz in chunks(d, p, end):
        if b + sz > end or not all(32 <= c < 127 for c in tag):
            return
        if tag == b"TXRC":
            t = read_txrc(d, b, sz)
            if t:
                yield t
        elif tag == b"FORM":
            yield from iter_txrc(d, b + 4 if d[b:b + 4] != b"TXRC" else b, b + sz)
        elif tag in (b"ULIB", b"UAST", b"UMTL", b"UFNT", b"LIST", b"CAT ", b"FMDL", b"MODL"):
            yield from iter_txrc(d, b, b + sz)


def xpr_header(t):
    x = t.data
    if len(x) < 16 or x[4:8] != b"XPR0":
        return
    _, hsize = struct.unpack_from("<II", x, 8)
    r = 16
    while r + 20 <= 4 + hsize:
        common = struct.unpack_from("<I", x, r)[0]
        if common == 0xFFFFFFFF:
            break
        typ = (common >> 16) & 7
        if typ == 4:  # texture
            _, data, _, fmt, size = struct.unpack_from("<5I", x, r)
            if size:  # linear (never DXT)
                return
            t.fmt = (fmt >> 8) & 0xFF
            t.mips = (fmt >> 16) & 15
            t.w, t.h = 1 << ((fmt >> 20) & 15), 1 << ((fmt >> 24) & 15)
            t.pixels = 4 + hsize + data
            return
        if typ == 3:  # palette
            r += 12
        else:
            return


def dxt_level_sizes(w, h, mips):
    out = []
    for _ in range(mips):
        out.append(max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16)
        w, h = max(1, w // 2), max(1, h // 2)
    return out


# ---- pictures ------------------------------------------------------------------


def bmp_rows(t):
    """(top-down rows of indices, one byte per texel, as bytes per row; palette RGB list)."""
    d = t.data
    off, hdr = struct.unpack_from("<II", d, 10)
    w, h, bpp = t.w, abs(t.h), t.bpp
    stride = ((w * bpp + 31) // 32) * 4
    ncol = struct.unpack_from("<I", d, 14 + 32)[0] or (1 << bpp)
    pal = [(d[14 + hdr + 4 * i + 2], d[14 + hdr + 4 * i + 1], d[14 + hdr + 4 * i]) for i in range(ncol)]
    pal += [(0, 0, 0)] * (256 - len(pal))
    rows = []
    for y in range(h):
        row = d[off + y * stride:off + y * stride + (w * bpp + 7) // 8]
        if bpp == 4:
            un = bytearray(w)
            un[0::2] = bytes(b >> 4 for b in row)[: (w + 1) // 2]
            un[1::2] = bytes(b & 15 for b in row)[: w // 2]
            row = bytes(un)
        rows.append(row)
    if t.h > 0:
        rows.reverse()
    return rows, pal


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def is_key(c):
    """The PS2 textures' colour key (magenta): transparent once loaded."""
    return c[0] >= 248 and c[1] <= 8 and c[2] >= 248


def cell(samples):
    """(mean RGB of the opaque samples or None, share of opaque samples)."""
    opaque = [c for c, a in samples if a]
    if not opaque:
        return None, 0.0
    k = len(opaque)
    return (sum(c[0] for c in opaque) / k, sum(c[1] for c in opaque) / k, sum(c[2] for c in opaque) / k), k / len(samples)


def ps2_thumb(rows, pal, n=8):
    h, w = len(rows), len(rows[0])
    out = []
    for gy in range(n):
        for gx in range(n):
            samples = []
            for sy in range(4):
                row = rows[min(h - 1, (gy * 4 + sy) * h // (n * 4))]
                for sx in range(4):
                    c = pal[row[min(w - 1, (gx * 4 + sx) * w // (n * 4))]]
                    samples.append((c, not is_key(c)))
            out.append(cell(samples))
    return out


def rgb565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def dxt3_decode(raw, w, h):
    """(RGB, opaque) rows of a small DXT3 level (opaque: alpha >= 8/15)."""
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    img = [[((0, 0, 0), False)] * (bw * 4) for _ in range(bh * 4)]
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * 16
            alpha = int.from_bytes(raw[o:o + 8], "little")
            c0, c1, bits = struct.unpack_from("<HHI", raw, o + 8)
            p0, p1 = rgb565(c0), rgb565(c1)
            pal = [p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)), tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))]
            for i in range(16):
                img[by * 4 + i // 4][bx * 4 + i % 4] = (pal[(bits >> (2 * i)) & 3], ((alpha >> (4 * i)) & 15) >= 8)
    return [row[:w] for row in img[:h]]


def xbox_thumb(t, n=8):
    sizes = dxt_level_sizes(t.w, t.h, t.mips)
    level, at = 0, t.pixels
    w, h = t.w, t.h
    # The smallest level at least n x n.
    while level + 1 < t.mips and min(w, h) // 2 >= n:
        at += sizes[level]
        level += 1
        w, h = w // 2, h // 2
    img = dxt3_decode(t.data[at:at + sizes[level]], w, h)
    out = []
    for gy in range(n):
        for gx in range(n):
            out.append(cell([img[y][x] for y in range(gy * h // n, max(gy * h // n + 1, (gy + 1) * h // n))
                             for x in range(gx * w // n, max(gx * w // n + 1, (gx + 1) * w // n))]))
    return out


def opaque_magenta(raw, w, h):
    """Texels of a DXT3 level that are opaque and magenta (the PS2 colour key
    left in some Xbox pictures: pink where the PS2 is transparent). A few
    are real art (pink details); more than 1/2048 of the texels is a key."""
    count = 0
    for o in range(0, max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * 16, 16):
        c0, c1, bits = struct.unpack_from("<HHI", raw, o + 8)
        pal = [rgb565(c0), rgb565(c1)]
        pal += [tuple((2 * a + b) // 3 for a, b in zip(*pal)), tuple((a + 2 * b) // 3 for a, b in zip(*pal))]
        keyed = [c[0] >= 192 and c[1] <= 64 and c[2] >= 192 for c in pal]  # magenta after DXT rounding
        if not any(keyed):
            continue
        alpha = int.from_bytes(raw[o:o + 8], "little")
        for i in range(16):
            if keyed[(bits >> (2 * i)) & 3] and ((alpha >> (4 * i)) & 15) >= 8:
                count += 1
    return count


def thumb_diff(a, b):
    """(mean RGB difference where both are opaque, mean difference of the opaque share)."""
    colour = [sum(abs(p - q) for p, q in zip(x[0], y[0])) / 3 for x, y in zip(a, b) if x[0] and y[0]]
    cover = sum(abs(x[1] - y[1]) for x, y in zip(a, b)) / len(a)
    return (sum(colour) / len(colour) if colour else 0.0), cover


# ---- matching --------------------------------------------------------------------


def key(name, slots):
    n = name.lower()
    if slots:
        m = re.search(r"id_?(\d)", n)
        if m:
            return "slot" + m.group(1)
    n = re.sub(r"_(64|128|256|512|1024)(?=_|$)", "", n)
    return re.sub(r"_+", "_", n)


def pairs_in(rel, ps2, xb):
    """[(ps2 tex, xbox tex, how)] for one file pair."""
    slots = rel.startswith("skaters/")
    by_name = {x.name.lower(): x for x in xb}
    xkeys = {}
    for x in xb:
        if not CROP.search(x.name):
            xkeys.setdefault(key(x.name, slots), x)
    out, used = [], set()
    for p in ps2:
        x = None
        how = "name"
        renamed = RENAMED.get((rel, p.name.lower()))
        if renamed:
            x, how = by_name.get(renamed), "renamed"
        else:
            x = xkeys.get(key(p.name, slots))
        if x is not None and id(x) not in used:
            used.add(id(x))
            out.append((p, x, how))
    return out


def ifind(root, rel):
    cur = root
    for part in rel.split("/"):
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = [n for n in names if n.lower() == part.lower()]
        if not hit:
            return None
        cur = os.path.join(cur, hit[0])
    return cur


def find_assets(root, markers):
    """The assets folder under root (root itself, root/assets, root/media/assets, any case)."""
    for rel in ("", "assets", "media/assets"):
        p = ifind(root, rel) if rel else root
        if p and os.path.isdir(p) and any(ifind(p, m) for m in markers):
            return p
    return None


# ---- pack ----------------------------------------------------------------------------


def pack_bytes(entries):
    """entries: [(tw, th, bits, hash, w, h, mips, payload, name)]"""
    head = struct.pack("<4sIII", b"PXTP", 1, len(entries), 0)
    table, blobs, placed = [], [], {}
    at = 16 + 64 * len(entries)
    for tw, th, bits, hsh, w, h, mips, payload, name in entries:
        offset = placed.get(id(payload))  # one copy for a picture several entries share
        if offset is None:
            at = (at + 15) & ~15
            offset = placed[id(payload)] = at
            blobs.append((at, payload))
            at += len(payload)
        table.append(struct.pack("<HHBBHQHHBBHII32s", tw, th, bits, 0, 0, hsh, w, h, mips, FORMAT_BC2, 0, offset,
                                 len(payload), name.encode("latin1")[:31]))
    out = bytearray(head + b"".join(table))
    for offset, payload in blobs:
        out += b"\0" * (offset - len(out))
        out += payload
    return bytes(out)


def copy_movies(ps2_assets, xbox_assets, out):
    """The Xbox Bink file of each PS2 movie (MOVIES/<path>.PSS|PSM -> Movies/<path>.bik, any case)."""
    movie_dir = os.path.join(out, "movies")
    if os.path.isdir(movie_dir):
        shutil.rmtree(movie_dir)
    ps2_movies = ifind(ps2_assets, "movies")
    copied, missing, size = [], [], 0
    if not ps2_movies:
        return copied, missing, size
    ps2_root = os.path.dirname(ps2_assets)
    for dirpath, dirnames, filenames in os.walk(ps2_movies):
        dirnames.sort()
        for f in sorted(filenames):
            if not f.lower().endswith((".pss", ".psm")):
                continue
            ps2_path = os.path.join(dirpath, f)
            rel = os.path.relpath(ps2_path, ps2_assets).replace(os.sep, "/")
            bink = ifind(xbox_assets, os.path.splitext(rel)[0] + ".bik")
            ps2_rel = os.path.relpath(ps2_path, ps2_root).replace(os.sep, "/").upper()
            if not bink:
                missing.append(ps2_rel)
                continue
            dst = os.path.join(movie_dir, *ps2_rel.split("/")) + ".bik"
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(bink, dst)
            copied.append(ps2_rel)
            size += os.path.getsize(dst)
    return copied, missing, size


def stamp_of(*roots):
    parts = []
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for f in sorted(filenames):
                if f.lower().endswith((".txr", ".zit", ".pss", ".psm", ".bik")):
                    st = os.stat(os.path.join(dirpath, f))
                    parts.append(f"{os.path.relpath(os.path.join(dirpath, f), root)} {st.st_size} {int(st.st_mtime)}")
    return f"xbox_pack 2 {fnv1a64(chr(10).join(parts).encode('utf-8')):016x}\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ps2", required=True)
    ap.add_argument("--xbox", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-diff", type=float, default=48.0, help="largest mean thumbnail difference (0-255) of a pair")
    ap.add_argument("--max-cover", type=float, default=0.5,
                    help="largest mean difference of the opaque share per thumbnail cell (0-1)")
    ap.add_argument("--no-movies", action="store_true", help="textures only")
    ap.add_argument("--force", action="store_true", help="rebuild even if the stamp matches")
    a = ap.parse_args()
    ps2_assets = find_assets(a.ps2, ("levels", "skaters"))
    xbox_assets = find_assets(a.xbox, ("levels", "skaters"))
    if not ps2_assets:
        sys.exit(f"xbox_pack: no PS2 ASSETS folder under {a.ps2}")
    if not xbox_assets:
        sys.exit(f"xbox_pack: no Xbox media/assets folder under {a.xbox}")
    ps2_root = os.path.dirname(ps2_assets)
    stamp = stamp_of(ps2_assets, xbox_assets) + ("textures only\n" if a.no_movies else "")
    stamp_file = os.path.join(a.out, ".stamp")
    if not a.force and os.path.exists(stamp_file) and open(stamp_file).read() == stamp:
        print(f"xbox_pack: {a.out} is up to date")
        return 0

    t0 = time.time()
    tex_dir = os.path.join(a.out, "textures")
    if os.path.isdir(tex_dir):
        for dirpath, _, filenames in os.walk(tex_dir, topdown=False):
            for f in filenames:
                os.remove(os.path.join(dirpath, f))
            os.rmdir(dirpath)
    os.makedirs(tex_dir)
    report = []
    totals = dict(files=0, ps2=0, packed=0, keys=0, rejected=0, ps2_texels=0, xbox_texels=0, bytes=0)
    for dirpath, dirnames, filenames in os.walk(xbox_assets):
        dirnames.sort()
        for f in sorted(filenames):
            if not f.lower().endswith((".txr", ".zit")):
                continue
            xpath = os.path.join(dirpath, f)
            rel = os.path.relpath(xpath, xbox_assets).replace(os.sep, "/").lower()
            ppath = ifind(ps2_assets, rel)
            if not ppath:
                continue
            ps2 = [t for t in iter_txrc(open(ppath, "rb").read()) if t.kind in ("TBMP", "TZMP") and t.bpp in (4, 8)]
            xb = [t for t in iter_txrc(open(xpath, "rb").read()) if t.kind == "TXPR"]
            entries, rejected, seen = [], [], set()
            texels = [0, 0]
            for p, x, how in pairs_in(rel, ps2, xb):
                if x.fmt != XBOX_DXT3 or x.mips < 1 or x.w * x.h < p.w * abs(p.h):
                    continue
                sizes = dxt_level_sizes(x.w, x.h, x.mips)
                payload = x.data[x.pixels:x.pixels + sum(sizes)]
                if len(payload) != sum(sizes):
                    rejected.append(f"{p.name} -> {x.name}: short mip chain")
                    continue
                rows, pal = bmp_rows(p)
                if len(set(b"".join(rows))) <= 1:
                    # One index everywhere: its key (size and index hash)
                    # is any flat texture's of that size, the game's
                    # run-time ones too ("Procedural:<n>", the juice bar).
                    rejected.append(f"{p.name} {p.w}x{abs(p.h)}: one colour, no key of its own")
                    continue
                diff, cover = thumb_diff(ps2_thumb(rows, pal), xbox_thumb(x))
                pink = opaque_magenta(payload, x.w, x.h)
                if pink > max(16, x.w * x.h // 2048):
                    rejected.append(f"{p.name} {p.w}x{abs(p.h)} -> {x.name} {x.w}x{x.h}: {pink} opaque magenta texels")
                    continue
                if diff > a.max_diff or cover > a.max_cover:
                    rejected.append(f"{p.name} {p.w}x{abs(p.h)} -> {x.name} {x.w}x{x.h}: pictures differ "
                                    f"(colour {diff:.1f}, cover {cover:.2f})")
                    continue
                hsh = fnv1a64(b"".join(rows))
                if (p.w, abs(p.h), hsh) in seen:
                    continue
                seen.add((p.w, abs(p.h), hsh))
                entries.append((p.w, abs(p.h), p.bpp, hsh, x.w, x.h, x.mips, payload, p.name))
                # The PS2 mip levels show the same picture (the GS samples
                # them with the same normalized coordinates).
                for level in p.levels:
                    indices = b"".join(bmp_rows(level)[0])
                    key = (level.w, abs(level.h), fnv1a64(indices))
                    if key not in seen and len(set(indices)) > 1:
                        seen.add(key)
                        entries.append(key[:2] + (level.bpp, key[2], x.w, x.h, x.mips, payload, p.name))
                texels[0] += p.w * abs(p.h)
                texels[1] += x.w * x.h
            if entries:
                ps2_rel = os.path.relpath(ppath, ps2_root).replace(os.sep, "/").upper()
                out = os.path.join(tex_dir, *ps2_rel.split("/")) + ".xtp"
                os.makedirs(os.path.dirname(out), exist_ok=True)
                data = pack_bytes(entries)
                with open(out, "wb") as fh:
                    fh.write(data)
                totals["files"] += 1
                totals["bytes"] += len(data)
                pictures = len({id(e[7]) for e in entries})
                report.append(f"{ps2_rel:40s} {len(ps2):5d} {pictures:5d} {len(entries):5d} {len(rejected):4d} "
                              f"{texels[1] / max(1, texels[0]):6.2f} {len(data) >> 10:8d}")
            for r in rejected:
                report.append(f"    rejected: {r}")
            totals["ps2"] += len(ps2)
            totals["packed"] += len({id(e[7]) for e in entries})
            totals["keys"] += len(entries)
            totals["rejected"] += len(rejected)
            totals["ps2_texels"] += texels[0]
            totals["xbox_texels"] += texels[1]
    head = [f"{'PS2 file':40s} {'ps2#':>5s} {'xbox':>5s} {'keys':>5s} {'rej':>4s} {'texels':>6s} {'KB':>8s}",
            "(ps2#: indexed PS2 textures in the file; xbox: replaced by an Xbox picture; keys: with the PS2 mip"
            " levels; texels: Xbox / PS2 texels of the replaced ones)"]
    summary = (f"{totals['packed']} Xbox textures ({totals['keys']} keys) for {totals['files']} PS2 files ({totals['ps2']} indexed PS2 textures "
               f"in the paired files, {totals['rejected']} pairs rejected), {totals['xbox_texels'] / max(1, totals['ps2_texels']):.2f}x "
               f"the texels, {totals['bytes'] >> 20} MB, {time.time() - t0:.1f} s")
    with open(os.path.join(tex_dir, "report.txt"), "w") as fh:
        fh.write("\n".join(head + report + ["", summary]) + "\n")
    print("xbox_pack:", summary)
    if a.no_movies:
        if os.path.isdir(os.path.join(a.out, "movies")):
            shutil.rmtree(os.path.join(a.out, "movies"))
    else:
        copied, missing, size = copy_movies(ps2_assets, xbox_assets, a.out)
        print(f"xbox_pack: {len(copied)} Xbox movies ({size >> 20} MB)"
              + (f"; none for {', '.join(missing)} (PS2 movie kept)" if missing else ""))
    with open(stamp_file, "w") as fh:
        fh.write(stamp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
