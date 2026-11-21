"""Aggressive Inline texture libraries (.TXR, and TXRC chunks inside .ZIT/.zif/.zal).

Both platforms use the same Z-Axis IFF layout:

    FORM <size>                      the file
      TXRC <size>                    one per texture
        TXRH 0x34                    u32 id, u16 width, u16 height (the PS2
                                     size, also kept on Xbox), 12 bytes 0,
                                     name (32 bytes, NUL padded)
        TBMP / TZMP <size>           PS2: a Windows BMP (4 or 8 bit, palette
                                     B,G,R,A with PS2 alpha 0..0x80)
        TTGA <size>                  PS2/Xbox: a 32-bit TGA (UI only)
        TXPR <size>                  Xbox: u32 0 + an XPR0 bundle (one
                                     D3DTexture resource, data at +0x800)
        TDDS <size>                  Xbox (DM2): u32 0 + a DDS file
                                     (DXT1/3/5 with its mips, or 32-bit)
      ZLZO <size>                    Xbox (BMX XXX): u32 unpacked size + an
                                     LZO1X stream of one chunk (a TXRC),
                                     zaxis_texdump.lzo1x_decompress

Disc files are data only: run with python3 -I. Needs numpy (decode) and
Pillow (PNG writing, only in the CLI paths).
"""

from __future__ import annotations

import os
import struct
import sys
from dataclasses import dataclass, field

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "share"))   # (tools/modding/share: zaxis_texdump.py)
import zaxis_texdump  # noqa: E402  (its LZO1X decoder)


@dataclass
class Texture:
    name: str
    tid: int  # TXRH id (a hash of the name; same on both platforms)
    lw: int  # TXRH width/height: the size the game was authored for
    lh: int
    kind: str  # TBMP, TZMP, TTGA, TXPR, TDDS
    data: bytes = field(repr=False)
    w: int = 0  # stored picture size
    h: int = 0
    fmt: str = ""
    mips: int = 1

    def rgba(self) -> np.ndarray:
        if self.kind in ("TBMP", "TZMP"):
            return decode_bmp(self.data)
        if self.kind == "TTGA":
            return decode_tga(self.data)
        if self.kind == "TXPR":
            return decode_xpr(self.data)[0]
        if self.kind == "TDDS":
            return decode_dds(self.data)
        raise ValueError(self.kind)


def chunks(d: bytes, p: int, end: int):
    while p + 8 <= end:
        tag = d[p : p + 4].decode("latin1")
        size = struct.unpack_from("<I", d, p + 4)[0]
        yield tag, p + 8, size
        p += 8 + size


def read_txrc(d: bytes, b: int, size: int) -> Texture | None:
    name, tid, lw, lh, pic, levels = "", 0, 0, 0, None, 0
    for tag, cb, cs in chunks(d, b, b + size):
        if tag == "TXRH":
            tid, lw, lh = struct.unpack_from("<IHH", d, cb)
            name = d[cb + 0x14 : cb + cs].split(b"\0")[0].decode("latin1")
        elif tag in ("TBMP", "TZMP", "TTGA", "TXPR", "TDDS"):
            # A PS2 picture is followed by its smaller mip levels (TBMPs of
            # half the size): the first is the picture.
            if pic is None:
                pic = (tag, d[cb : cb + cs])
            if tag == pic[0]:
                levels += 1
    if pic is None:
        return None
    t = Texture(name, tid, lw, lh, pic[0], pic[1])
    describe(t)
    if t.kind in ("TBMP", "TZMP"):
        t.mips = levels
    return t


def iter_txrc(d: bytes, p: int = 0, end: int | None = None):
    """Every TXRC chunk anywhere in an IFF tree (TXR, ZIT, zif, ZAL entries)."""
    end = len(d) if end is None else end
    for tag, b, sz in chunks(d, p, end):
        if b + sz > end or not tag.isprintable():
            return
        if tag == "TXRC":
            t = read_txrc(d, b, sz)
            if t:
                yield t
        elif tag == "FORM":
            yield from iter_txrc(d, b + 4 if d[b : b + 4] not in (b"TXRC", b"ZLZO") else b, b + sz)
        elif tag in ("ULIB", "UAST", "UMTL", "UFNT", "LIST", "CAT ", "FMDL", "MODL"):
            yield from iter_txrc(d, b, b + sz)
        elif tag == "ZLZO":
            u = zaxis_texdump.lzo1x_decompress(d[b + 4 : b + sz], struct.unpack_from("<I", d, b)[0])
            yield from iter_txrc(u, 0, len(u))


def read_txr(path: str) -> list[Texture]:
    d = open(path, "rb").read()
    if d[:4] != b"FORM":
        raise ValueError(f"{path}: not FORM")
    out = []
    for tag, b, sz in chunks(d, 8, 8 + struct.unpack_from("<I", d, 4)[0]):
        if tag == "TXRC":
            t = read_txrc(d, b, sz)
            if t:
                out.append(t)
    return out


# ---- descriptions -----------------------------------------------------------

XBOX_FORMATS = {
    0x00: "L8", 0x01: "AL8", 0x02: "A1R5G5B5", 0x03: "X1R5G5B5", 0x04: "A4R4G4B4",
    0x05: "R5G6B5", 0x06: "A8R8G8B8", 0x07: "X8R8G8B8", 0x0B: "P8", 0x0C: "DXT1",
    0x0E: "DXT3", 0x0F: "DXT5", 0x10: "LIN_A1R5G5B5", 0x11: "LIN_R5G6B5",
    0x12: "LIN_A8R8G8B8", 0x13: "LIN_L8", 0x19: "A8", 0x1A: "A8L8", 0x1D: "LIN_A4R4G4B4",
    0x1E: "LIN_X8R8G8B8", 0x3F: "LIN_A8B8G8R8",
}


def xpr_header(x: bytes):
    """(format code, width, height, mips, data offset, palette or None)."""
    assert x[4:8] == b"XPR0", x[:8]
    total, hsize = struct.unpack_from("<II", x, 8)
    r = 16
    tex, pal = None, None
    while r + 4 <= hsize:
        common = struct.unpack_from("<I", x, r)[0]
        if common == 0xFFFFFFFF:
            break
        typ = (common >> 16) & 7
        if typ == 4:  # texture
            _, data, _, fmt, size = struct.unpack_from("<5I", x, r)
            code = (fmt >> 8) & 0xFF
            if size:  # linear: explicit size
                w = (size & 0xFFF) + 1
                h = ((size >> 12) & 0xFFF) + 1
            else:
                w = 1 << ((fmt >> 20) & 15)
                h = 1 << ((fmt >> 24) & 15)
            tex = (code, w, h, (fmt >> 16) & 15, data)
            r += 20
        elif typ == 3:  # palette
            _, data, _ = struct.unpack_from("<3I", x, r)
            n = {0: 256, 1: 128, 2: 64, 3: 32}[common >> 30]
            pal = (data, n)
            r += 12
        else:
            break
    return tex, pal, 4 + hsize


def describe(t: Texture):
    d = t.data
    if t.kind in ("TBMP", "TZMP"):
        _, _, _, _, off, _, w, h, _, bpp = struct.unpack_from("<2sIHHIIiiHH", d, 0)
        t.w, t.h, t.fmt = w, abs(h), f"BMP{bpp}"
    elif t.kind == "TTGA":
        t.w, t.h = struct.unpack_from("<HH", d, 12)
        t.fmt = f"TGA{d[16]}"
    elif t.kind == "TXPR":
        tex, pal, _ = xpr_header(d)
        code, w, h, mips, _ = tex
        t.w, t.h, t.mips = w, h, mips
        t.fmt = XBOX_FORMATS.get(code, hex(code))
    elif t.kind == "TDDS":
        w, h, mips, fmt, _ = dds_header(t.data)
        t.w, t.h, t.mips, t.fmt = w, h, mips, fmt


# ---- decoders ---------------------------------------------------------------


def decode_bmp(d: bytes) -> np.ndarray:
    _, _, _, _, off, hdr, w, h, _, bpp = struct.unpack_from("<2sIHHIIiiHH", d, 0)
    ncol = struct.unpack_from("<I", d, 14 + 32)[0] or (1 << bpp if bpp <= 8 else 0)
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    rows = np.frombuffer(d, np.uint8, stride * h, off).reshape(h, stride)
    if bpp in (24, 32):
        n = bpp // 8
        px = rows[:, : w * n].reshape(h, w, n)
        img = np.empty((h, w, 4), np.uint8)
        img[..., 0], img[..., 1], img[..., 2] = px[..., 2], px[..., 1], px[..., 0]
        img[..., 3] = 255 if n == 3 else np.minimum(px[..., 3].astype(np.int32) * 2, 255)
        return img if top_down else img[::-1]
    if bpp == 8:
        idx = rows[:, :w]
    elif bpp == 4:
        idx = np.empty((h, stride * 2), np.uint8)
        idx[:, 0::2] = rows >> 4
        idx[:, 1::2] = rows & 15
        idx = idx[:, :w]
    else:
        raise ValueError(f"bmp {bpp} bit")
    pal = np.frombuffer(d, np.uint8, 4 * ncol, 14 + hdr).reshape(-1, 4)
    rgba = np.stack([pal[:, 2], pal[:, 1], pal[:, 0], pal[:, 3]], 1).copy()
    # PS2 alpha: 0x80 is opaque. Palettes with all-zero alpha are opaque.
    a = rgba[:, 3].astype(np.int32)
    if a.max() == 0:
        a[:] = 255
    else:
        a = np.minimum(a * 2, 255)
    rgba[:, 3] = a
    img = rgba[idx]
    return img if top_down else img[::-1]


def decode_tga(d: bytes) -> np.ndarray:
    w, h = struct.unpack_from("<HH", d, 12)
    bpp = d[16]
    off = 18 + d[0]
    if d[2] != 2 or bpp not in (24, 32):
        raise ValueError("tga type")
    n = bpp // 8
    px = np.frombuffer(d, np.uint8, w * h * n, off).reshape(h, w, n)
    out = np.empty((h, w, 4), np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = px[..., 2], px[..., 1], px[..., 0]
    out[..., 3] = px[..., 3] if n == 4 else 255
    return out if d[17] & 0x20 else out[::-1]


def _rgb565(c: np.ndarray) -> np.ndarray:
    r = ((c >> 11) & 31).astype(np.uint32)
    g = ((c >> 5) & 63).astype(np.uint32)
    b = (c & 31).astype(np.uint32)
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1)


def _dxt_colors(blocks: np.ndarray, off: int, four: bool) -> np.ndarray:
    """(n,16,3) colours of the colour part at byte off of each block."""
    c0 = blocks[:, off].astype(np.uint16) | (blocks[:, off + 1].astype(np.uint16) << 8)
    c1 = blocks[:, off + 2].astype(np.uint16) | (blocks[:, off + 3].astype(np.uint16) << 8)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    four_mode = np.ones(len(blocks), bool) if four else (c0 > c1)
    p2 = np.where(four_mode[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four_mode[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # n,4,3
    bits = blocks[:, off + 4 : off + 8].astype(np.uint32)
    word = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    idx = (word[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    cols = np.take_along_axis(pal, idx[..., None].astype(np.int64), 1)
    alpha1 = np.where((~four_mode)[:, None] & (idx == 3), 0, 255)
    return cols, alpha1


def decode_dxt(raw: bytes, w: int, h: int, kind: str) -> np.ndarray:
    bw, bh = max(1, (w + 3) // 4), max(1, (h + 3) // 4)
    bs = 8 if kind == "DXT1" else 16
    blocks = np.frombuffer(raw, np.uint8, bw * bh * bs).reshape(-1, bs)
    if kind == "DXT1":
        cols, a = _dxt_colors(blocks, 0, False)
    else:
        cols, _ = _dxt_colors(blocks, 8, True)
        if kind == "DXT3":
            ab = blocks[:, :8]
            lo, hi = ab & 15, ab >> 4
            a = np.empty((len(blocks), 16), np.uint32)
            a[:, 0::2], a[:, 1::2] = lo * 17, hi * 17
        else:  # DXT5
            a0 = blocks[:, 0].astype(np.int32)
            a1 = blocks[:, 1].astype(np.int32)
            bits = np.zeros(len(blocks), np.uint64)
            for i in range(6):
                bits |= blocks[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
            idx = ((bits[:, None] >> (3 * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int32)
            pal = np.zeros((len(blocks), 8), np.int32)
            pal[:, 0], pal[:, 1] = a0, a1
            big = a0 > a1
            for i in range(1, 7):
                pal[:, i + 1] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
            for i in range(1, 5):
                pal[:, i + 1] = np.where(big, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
            pal[:, 6] = np.where(big, pal[:, 6], 0)
            pal[:, 7] = np.where(big, pal[:, 7], 255)
            a = np.take_along_axis(pal, idx, 1)
    px = np.concatenate([cols.astype(np.uint8), np.asarray(a, np.uint8)[..., None]], -1)
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w]


def unswizzle(raw: np.ndarray, w: int, h: int) -> np.ndarray:
    """Xbox swizzled (Morton, x in the low bit) to linear, any w/h power of two."""
    xs = np.arange(w)
    ys = np.arange(h)
    xo = np.zeros(w, np.int64)
    yo = np.zeros(h, np.int64)
    bit, sx, sy = 0, 0, 0
    while (1 << sx) < w or (1 << sy) < h:
        if (1 << sx) < w:
            xo |= ((xs >> sx) & 1) << bit
            sx += 1
            bit += 1
        if (1 << sy) < h:
            yo |= ((ys >> sy) & 1) << bit
            sy += 1
            bit += 1
    return raw[yo[:, None] + xo[None, :]]


def dds_header(x: bytes):
    """(width, height, mips, format, data offset) of a TDDS body (u32 0, then 'DDS ')."""
    o = x.find(b"DDS ", 0, 16)
    assert o >= 0, x[:16]
    _, _, flags, h, w, _, _, mips = struct.unpack_from("<4s7I", x, o)
    pf_flags, fourcc, bits = struct.unpack_from("<I4sI", x, o + 4 + 72 + 4)
    if pf_flags & 4:
        fmt = fourcc.decode("latin1")
    else:
        fmt = f"RGB{bits}" if not pf_flags & 1 else f"ARGB{bits}"
    return w, h, max(1, mips), fmt, o + 128


def decode_dds(x: bytes) -> np.ndarray:
    w, h, _, fmt, off = dds_header(x)
    raw = x[off:]
    if fmt in ("DXT1", "DXT3", "DXT5"):
        return decode_dxt(raw, w, h, fmt)
    if fmt in ("ARGB32", "RGB32"):
        px = np.frombuffer(raw, np.uint8, w * h * 4).reshape(h, w, 4)
        out = px[..., [2, 1, 0, 3]].copy()
        if fmt == "RGB32":
            out[..., 3] = 255
        return out
    raise ValueError(f"dds format {fmt}")


def decode_xpr(x: bytes):
    tex, pal, base = xpr_header(x)
    code, w, h, mips, data = tex
    kind = XBOX_FORMATS.get(code, hex(code))
    raw = x[base + data :]
    if kind in ("DXT1", "DXT3", "DXT5"):
        return decode_dxt(raw, w, h, kind), kind
    if kind in ("A8R8G8B8", "X8R8G8B8", "LIN_A8R8G8B8", "LIN_X8R8G8B8"):
        px = np.frombuffer(raw, np.uint8, w * h * 4).reshape(-1, 4)
        if not kind.startswith("LIN"):
            px = unswizzle(px, w, h)
        px = px.reshape(h, w, 4)
        out = px[..., [2, 1, 0, 3]].copy()
        if "X8" in kind:
            out[..., 3] = 255
        return out, kind
    if kind == "P8":
        idx = np.frombuffer(raw, np.uint8, w * h)
        idx = unswizzle(idx, w, h).reshape(h, w)
        pdata, n = pal
        p = np.frombuffer(x, np.uint8, 4 * n, base + pdata).reshape(-1, 4)[:, [2, 1, 0, 3]]
        return p[idx], kind
    if kind in ("R5G6B5", "A1R5G5B5", "A4R4G4B4", "X1R5G5B5"):
        v = np.frombuffer(raw, "<u2", w * h)
        v = unswizzle(v, w, h).reshape(h, w).astype(np.uint32)
        if kind == "R5G6B5":
            rgb = _rgb565(v)
            a = np.full((h, w), 255)
        elif kind == "A4R4G4B4":
            rgb = np.stack([(v >> 8) & 15, (v >> 4) & 15, v & 15], -1) * 17
            a = (v >> 12) * 17
        else:
            rgb = np.stack([(v >> 10) & 31, (v >> 5) & 31, v & 31], -1)
            rgb = (rgb << 3) | (rgb >> 2)
            a = np.where(v >> 15, 255, 0) if kind == "A1R5G5B5" else np.full((h, w), 255)
        return np.concatenate([rgb, a[..., None]], -1).astype(np.uint8), kind
    raise ValueError(f"xbox format {kind}")


# ---- CLI --------------------------------------------------------------------

if __name__ == "__main__":
    import argparse
    import os

    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--png", help="write every texture as PNG into this directory")
    a = ap.parse_args()
    d = open(a.file, "rb").read()
    texs = list(iter_txrc(d))
    for t in texs:
        print(f"{t.tid:08x} {t.name:32s} logical {t.lw}x{t.lh} stored {t.w}x{t.h} {t.fmt} mips {t.mips}")
    if a.png:
        from PIL import Image

        os.makedirs(a.png, exist_ok=True)
        for t in texs:
            Image.fromarray(t.rgba()).save(os.path.join(a.png, f"{t.name}.png"))
    print(len(texs), "textures", file=sys.stderr)
