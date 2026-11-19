#!/usr/bin/env python3
"""Dump a CT32 frame buffer region from a runtime GS VRAM snapshot to PPM.
usage: vram2ppm.py <vram.bin> <fbp pages> <width> <height> <out.ppm> [alpha]
Assumes the runtime's linear layout (fbp*8192 + (y*width + x)*4)."""
import sys
v = open(sys.argv[1], 'rb').read()
fbp, w, h = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
alpha = len(sys.argv) > 6
base = fbp * 8192
out = bytearray(b'P6 %d %d 255\n' % (w, h))
for y in range(h):
    row = v[base + y * w * 4: base + (y + 1) * w * 4]
    for x in range(w):
        r, g, b, a = row[x*4:x*4+4]
        out += bytes((a, a, a)) if alpha else bytes((r, g, b))
open(sys.argv[5], 'wb').write(out)
