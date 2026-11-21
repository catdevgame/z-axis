#!/usr/bin/env python3
"""The Z-Axis Collection's app icon (app-icon.png): our own drawing, no game art.

A rounded square of deep blue with a slanted Z: two light bars and an orange stroke between them.
Plain Python (no imaging library), 3x3 samples a pixel.

usage: app_icon.py [out.png] [size]
"""
import struct
import sys
import zlib

OUT = sys.argv[1] if len(sys.argv) > 1 else 'app-icon.png'
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
SAMPLES = 3

TOP, BOTTOM = (0x1d, 0x35, 0x57), (0x0b, 0x13, 0x24)
LIGHT, ORANGE = (0xec, 0xf0, 0xf3), (0xf7, 0x8c, 0x1f)


def quad(points):
    """A convex quadrilateral (clockwise in y-down) as its four edges' half-plane tests."""
    edges = []
    for i in range(4):
        (x0, y0), (x1, y1) = points[i], points[(i + 1) % 4]
        edges.append((y1 - y0, x0 - x1, x1 * y0 - x0 * y1))
    return edges


def inside(edges, x, y):
    return all(a * x + b * y + c <= 0 for a, b, c in edges)


# Unit square coordinates (0..1), the Z slanted to the right.
S = 0.10  # slant
SHAPES = [
    (quad([(0.26 + S, 0.22), (0.80 + S, 0.22), (0.78 + S, 0.36), (0.24 + S, 0.36)]), LIGHT),   # top bar
    (quad([(0.21 - S, 0.64), (0.75 - S, 0.64), (0.73 - S, 0.78), (0.19 - S, 0.78)]), LIGHT),   # bottom bar
    (quad([(0.62 + S * 0.6, 0.30), (0.80 + S * 0.6, 0.30), (0.38 - S * 0.6, 0.70), (0.20 - S * 0.6, 0.70)]), ORANGE),
]
RADIUS = 0.18


def in_rounded_square(x, y):
    cx = min(max(x, RADIUS), 1 - RADIUS)
    cy = min(max(y, RADIUS), 1 - RADIUS)
    return (x - cx) ** 2 + (y - cy) ** 2 <= RADIUS ** 2


def pixel(px, py):
    r = g = b = a = 0.0
    n = SAMPLES * SAMPLES
    for sy in range(SAMPLES):
        for sx in range(SAMPLES):
            x = (px + (sx + 0.5) / SAMPLES) / SIZE
            y = (py + (sy + 0.5) / SAMPLES) / SIZE
            if not in_rounded_square(x, y):
                continue
            colour = tuple(TOP[i] + (BOTTOM[i] - TOP[i]) * y for i in range(3))
            for edges, fill in SHAPES:
                if inside(edges, x, y):
                    colour = fill
            r += colour[0]
            g += colour[1]
            b += colour[2]
            a += 1
    if a == 0:
        return b'\0\0\0\0'
    return bytes((int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5), int(255 * a / n + 0.5)))


def main():
    rows = []
    for py in range(SIZE):
        rows.append(b'\0' + b''.join(pixel(px, py) for px in range(SIZE)))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', SIZE, SIZE, 8, 6, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))
    with open(OUT, 'wb') as f:
        f.write(png)


if __name__ == '__main__':
    main()
