"""Match Xbox and PS2 textures (TXR, ZIT, zif) and report sizes; optional
side-by-side sheets (PS2 nearest-upscaled to the Xbox size | Xbox).

Matching, per file pair:
  1. TXRH name (case-insensitive, size tokens like _256/_512 dropped);
  2. skater body slots by the _IDn token (grob_256_ID2 <-> grob_512_ID2);
  3. what is left, by picture: both downscaled to 16x16, greedy smallest
     mean colour difference under a threshold (renamed textures).

    .venv/bin/python -I tools/compare_textures.py --json /tmp/ai-xbox-compare/textures.json
    .venv/bin/python -I tools/compare_textures.py --sheets /tmp/ai-xbox-compare

Disc files are data only. Output never goes into git.
"""

from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import aitxr  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env)
PS2 = os.path.join(zpaths.AI_WORKSPACE, "recomp", "disc-root", "ASSETS")
XBOX = os.path.join(zpaths.XBOX_DATA, "disc", "media", "assets")


def ifind(root: str, rel: str) -> str | None:
    """Case-insensitive path lookup (PS2 names are upper case 8.3)."""
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


def pairs():
    """(rel, ps2 path or None, xbox path) for every texture-bearing Xbox file."""
    out = []
    for x in sorted(glob.glob(os.path.join(XBOX, "**", "*"), recursive=True)):
        if os.path.splitext(x)[1].lower() not in (".txr", ".zit", ".zif"):
            continue
        rel = os.path.relpath(x, XBOX)
        out.append((rel, ifind(PS2, rel), x))
    return out


def load(path: str):
    return list(aitxr.iter_txrc(open(path, "rb").read()))


def key(name: str, slots: bool) -> str:
    n = name.lower()
    if slots:
        m = re.search(r"id_?(\d)", n)
        if m:
            return "slot" + m.group(1)
    n = re.sub(r"_(64|128|256|512|1024)(?=_|$)", "", n)
    return re.sub(r"_+", "_", n)


def thumb(t, size=16):
    try:
        img = t.rgba().astype(np.float32)
    except Exception:  # noqa: BLE001
        return None
    h, w = img.shape[:2]
    ys = (np.arange(size) * h // size)
    xs = (np.arange(size) * w // size)
    acc = np.zeros((size, size, 4), np.float32)
    for oy in range(4):
        for ox in range(4):
            acc += img[np.minimum(ys + oy * h // (size * 4), h - 1)][:, np.minimum(xs + ox * w // (size * 4), w - 1)]
    return acc / 16


def match(ps2: list, xb: list, slots: bool):
    """[(ps2 tex, xbox tex, how)], xbox-only, ps2-only."""
    pm = collections.OrderedDict()
    for t in ps2:
        pm.setdefault(key(t.name, slots), t)
    out, used_p, used_x = [], set(), set()
    for x in xb:
        k = key(x.name, slots)
        if k in pm and id(pm[k]) not in used_p:
            out.append((pm[k], x, "name"))
            used_p.add(id(pm[k]))
            used_x.add(id(x))
    rest_p = [t for t in ps2 if id(t) not in used_p]
    rest_x = [t for t in xb if id(t) not in used_x]
    if rest_p and rest_x and len(rest_p) * len(rest_x) <= 40000:
        tp = [(t, thumb(t)) for t in rest_p]
        tx = [(t, thumb(t)) for t in rest_x]
        cand = []
        for i, (p, a) in enumerate(tp):
            for j, (x, b) in enumerate(tx):
                if a is None or b is None:
                    continue
                cand.append((float(np.abs(a[..., :3] - b[..., :3]).mean()), i, j))
        cand.sort()
        di, dj = set(), set()
        for err, i, j in cand:
            if err > 12 or i in di or j in dj:
                continue
            di.add(i)
            dj.add(j)
            out.append((tp[i][0], tx[j][0], f"picture({err:.1f})"))
        rest_p = [tp[i][0] for i in range(len(tp)) if i not in di]
        rest_x = [tx[j][0] for j in range(len(tx)) if j not in dj]
    return out, rest_x, rest_p


def sheet(matched, out: str, title: str):
    from PIL import Image, ImageDraw

    tiles = []
    for p, x, how in matched:
        try:
            pi, xi = p.rgba(), x.rgba()
        except Exception as e:  # noqa: BLE001
            print("skip", p.name, e, file=sys.stderr)
            continue
        h, w = xi.shape[:2]
        ph, pw = pi.shape[:2]
        up = pi[(np.arange(h) * ph // h)][:, (np.arange(w) * pw // w)]
        scale = max(1, 256 // max(h, w))
        pair = np.concatenate([up, np.zeros((h, max(2, w // 64), 4), np.uint8), xi], 1)
        img = Image.fromarray(pair, "RGBA")
        if scale > 1:
            img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
        bg = Image.new("RGBA", img.size, (64, 64, 64, 255))
        bg.alpha_composite(img)
        lab = Image.new("RGBA", (max(bg.width, 520), 18), (20, 20, 20, 255))
        ImageDraw.Draw(lab).text(
            (4, 3), f"PS2 {p.name} {p.w}x{p.h} {p.fmt}  |  Xbox {x.name} {x.w}x{x.h} {x.fmt}  [{how}]", fill=(230, 230, 230, 255))
        col = Image.new("RGBA", (lab.width, bg.height + 18), (12, 12, 12, 255))
        col.paste(lab, (0, 0))
        col.paste(bg, (0, 18))
        tiles.append(col)
    if not tiles:
        return
    W = max(t.width for t in tiles)
    H = sum(t.height + 6 for t in tiles)
    canvas = Image.new("RGBA", (W, H + 24), (12, 12, 12, 255))
    ImageDraw.Draw(canvas).text((6, 6), title + "   (left: PS2 nearest-upscaled to the Xbox size, right: Xbox)", fill=(255, 255, 0, 255))
    y = 24
    for t in tiles:
        canvas.paste(t, (0, y))
        y += t.height + 6
    canvas.convert("RGB").save(out, optimize=True)
    print("wrote", out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sheets", help="directory for comparison images")
    ap.add_argument("--json", help="write per-texture rows here")
    ap.add_argument("--filter", default="", help="only files whose path contains this")
    ap.add_argument("--per-sheet", type=int, default=8)
    a = ap.parse_args()
    report = {}
    tot = collections.Counter()
    print(f"{'file':40s} {'ps2#':>5s} {'xb#':>5s} {'match':>5s} {'byPic':>5s} {'x-only':>6s} {'p-only':>6s} {'bigger':>6s} {'px x/p':>6s}  ps2 bytes -> xbox bytes")
    for rel, p, x in pairs():
        if a.filter and a.filter.lower() not in rel.lower():
            continue
        xb = load(x)
        if not xb:
            continue
        ps2 = load(p) if p else []
        slots = rel.lower().startswith("skaters/")
        m, xonly, ponly = match(ps2, xb, slots)
        bigger = sum(1 for pp, xx, _ in m if xx.w * xx.h > pp.w * pp.h)
        bypic = sum(1 for _, _, how in m if how != "name")
        px_p = sum(pp.w * pp.h for pp, _, _ in m)
        px_x = sum(xx.w * xx.h for _, xx, _ in m)
        bp = sum(len(t.data) for t in ps2)
        bx = sum(len(t.data) for t in xb)
        print(f"{rel:40s} {len(ps2):5d} {len(xb):5d} {len(m):5d} {bypic:5d} {len(xonly):6d} {len(ponly):6d} {bigger:6d} {px_x / max(1, px_p):6.2f}  {bp:>9d} -> {bx:>9d}")
        lr = rel.lower()
        if lr.startswith("levels/") and "/mp/" not in lr and "zoo" not in lr:
            tot["levels_ps2"] += len(ps2)
            tot["levels_xb"] += len(xb)
            tot["levels_match"] += len(m)
            tot["levels_bigger"] += bigger
            tot["levels_px_p"] += px_p
            tot["levels_px_x"] += px_x
        report[rel] = dict(
            ps2=p, xbox=x,
            matched=[dict(ps2=pp.name, ps2_id=f"{pp.tid:08x}", ps2_size=f"{pp.w}x{pp.h}", ps2_fmt=pp.fmt,
                          xbox=xx.name, xbox_id=f"{xx.tid:08x}", xbox_size=f"{xx.w}x{xx.h}", xbox_fmt=xx.fmt,
                          xbox_mips=xx.mips, logical=f"{pp.lw}x{pp.lh}", xbox_logical=f"{xx.lw}x{xx.lh}", how=how)
                     for pp, xx, how in m],
            xbox_only=[f"{t.name} {t.w}x{t.h}" for t in xonly], ps2_only=[f"{t.name} {t.w}x{t.h}" for t in ponly])
        if a.sheets and m and "/mp/" not in lr and "zoo" not in lr:
            os.makedirs(a.sheets, exist_ok=True)
            pick = sorted(m, key=lambda r: -(r[1].w * r[1].h) / max(1, r[0].w * r[0].h))[: a.per_sheet]
            if (pick[0][1].w * pick[0][1].h) > (pick[0][0].w * pick[0][0].h):
                sheet(pick, os.path.join(a.sheets, "tex_" + rel.replace("/", "_").replace(" ", "_") + ".png"), rel)
    print(dict(tot))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(report, f, indent=1)


if __name__ == "__main__":
    main()
