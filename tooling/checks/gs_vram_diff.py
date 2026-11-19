#!/usr/bin/env python3
"""Compares two GS local memory dumps (4 MiB, from ps2xGsReplay --dump-vram).

    gs_vram_diff.py <a.vram> <b.vram> [max-pages]

Lists the 8 KiB GS pages that differ (page number, block base, number of
differing bytes, first differing offset) and the total.
"""
import sys

PAGE = 8192


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 2
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 64
    if len(a) != len(b):
        print(f'size differs: {len(a)} vs {len(b)}')
        return 1
    total = 0
    pages = 0
    for page in range(len(a) // PAGE):
        pa = a[page * PAGE:(page + 1) * PAGE]
        pb = b[page * PAGE:(page + 1) * PAGE]
        if pa == pb:
            continue
        diff = [i for i in range(PAGE) if pa[i] != pb[i]]
        total += len(diff)
        pages += 1
        if pages <= limit:
            print(f'page {page:3d} (bp {page * 32:5d}): {len(diff):5d} bytes differ, first at +0x{diff[0]:04x}')
    print(f'{pages} pages, {total} bytes differ')
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
