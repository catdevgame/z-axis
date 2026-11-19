"""Prototype of the "Xbox textures" pack the port would build from the
user's two discs: for every PS2 texture with a higher-resolution Xbox
counterpart, the Xbox picture keyed by what the port sees at load time.

Output (never in git; default /tmp/ai-xbox-compare/pack):
    <out>/manifest.json   {"<ps2 file>": [{ps2_name, ps2_id, ps2_size,
                           ps2_pixels_fnv, xbox_file, xbox_name, size,
                           mips, how, png}]}
    <out>/<ps2 file>/<ps2_name>.png   (with --png; RGBA, mip 0)

ps2_pixels_fnv: FNV-1a 64 of the PS2 BMP's index data in top-down row
order (the order the game copies into its texture object, +0x40) so a
loader can confirm it has the same picture before swapping.

The port itself would not need PNGs: the Xbox data is DXT3 (BC2) with a
full mip chain, which Metal/D3D12/Vulkan sample directly; the pack can be
built in memory at startup from the XISO (about 0.3 s for all levels).

    .venv/bin/python -I tools/xbox_texture_pack.py [--png] [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aitxr  # noqa: E402
import compare_textures as ct  # noqa: E402


def fnv64(data: bytes) -> str:
    h = 0xCBF29CE484222325
    # numpy-free but fast enough for a prototype: process 8 bytes at a time
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"{h:016x}"


def bmp_indices(t: aitxr.Texture) -> bytes:
    d = t.data
    _, _, _, _, off, hdr, w, h, _, bpp = struct.unpack_from("<2sIHHIIiiHH", d, 0)
    stride = ((w * bpp + 31) // 32) * 4
    rows = np.frombuffer(d, np.uint8, stride * abs(h), off).reshape(abs(h), stride)[:, : (w * bpp + 7) // 8]
    if h > 0:
        rows = rows[::-1]
    return rows.tobytes()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="/tmp/ai-xbox-compare/pack")
    ap.add_argument("--png", action="store_true")
    ap.add_argument("--hash", action="store_true", help="add ps2_pixels_fnv (slow in pure Python)")
    a = ap.parse_args()
    t0 = time.time()
    manifest = {}
    n = 0
    decode_s = 0.0
    for rel, p, x in ct.pairs():
        lr = rel.lower()
        if not p or "zoo" in lr:
            continue
        ps2, xb = ct.load(p), ct.load(x)
        m, _, _ = ct.match(ps2, xb, lr.startswith("skaters/"))
        rows = []
        for pt, xt, how in m:
            if xt.w * xt.h <= pt.w * pt.h or xt.kind != "TXPR":
                continue
            row = dict(ps2_name=pt.name, ps2_id=f"{pt.tid:08x}", ps2_size=f"{pt.w}x{pt.h}", ps2_fmt=pt.fmt,
                       xbox_file=rel, xbox_name=xt.name, size=f"{xt.w}x{xt.h}", fmt=xt.fmt, mips=xt.mips, how=how)
            if a.hash and pt.kind in ("TBMP", "TZMP"):
                row["ps2_pixels_fnv"] = fnv64(bmp_indices(pt))
            if a.png:
                from PIL import Image

                d0 = time.time()
                img = xt.rgba()
                decode_s += time.time() - d0
                dst = os.path.join(a.out, os.path.relpath(p, ct.PS2))
                os.makedirs(dst, exist_ok=True)
                fn = os.path.join(dst, f"{pt.name}.png")
                Image.fromarray(img).save(fn)
                row["png"] = fn
            rows.append(row)
            n += 1
        if rows:
            manifest[os.path.relpath(p, ct.PS2)] = rows
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"{n} replacement textures from {len(manifest)} PS2 files in {time.time() - t0:.2f} s"
          + (f" (DXT3 decode {decode_s:.2f} s)" if a.png else ""))


if __name__ == "__main__":
    main()
