#!/usr/bin/env python3
"""Source file paths (__FILE__ strings) left in a Z-Axis Xbox build, and who uses them.

    python3 -I tools/xbe_paths.py extract <Xbox image> <out default.xbe>
    python3 -I tools/xbe_paths.py paths <default.xbe> <out.json>

extract: copies default.xbe out of an XISO (or a full image with the game
partition at a known offset) with tools/modding/
xbox_files.py's reader. The image is opened read only.

paths: every NUL-terminated string in the XBE that looks like a C source path
(".c", ".cpp", ".h" with a slash), each with its virtual address and the
virtual addresses of the instructions that push or move it (x86 `push imm32`
68 xx xx xx xx, `mov r32, imm32` B8+r, `mov [esp+n], imm32` C7 44 24 nn, `mov
[r32+d], imm32` C7 4r / C7 8r: the engine's error reports store __FILE__ at
[eax+0x14]), so a path can be tied to the functions around those uses.

Data only; the output is derived from the disc: write it to scratch, never commit it.
"""
import json
import os
import re
import struct
import sys

PATH_RE = re.compile(rb'[A-Za-z0-9_.:\\/ -]{3,200}\.(?:c|cpp|h)\x00', re.I)


class XBE:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        assert d[:4] == b'XBEH', 'not an XBE'
        self.base = struct.unpack_from('<I', d, 0x104)[0]
        n = struct.unpack_from('<I', d, 0x11C)[0]
        sh = struct.unpack_from('<I', d, 0x120)[0] - self.base
        self.secs = []
        for i in range(n):
            fl, va, vs, ra, rs, name = struct.unpack_from('<IIIIII', d, sh + i * 56)
            nm = d[name - self.base:d.index(b'\0', name - self.base)].decode('latin1')
            self.secs.append((nm, va, vs, ra, rs, fl))

    def va(self, o):
        for nm, va, vs, ra, rs, fl in self.secs:
            if ra <= o < ra + rs:
                return va + o - ra
        return None

    def section_of(self, o):
        for nm, va, vs, ra, rs, fl in self.secs:
            if ra <= o < ra + rs:
                return nm
        return None


def extract(iso, out):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'modding'))   # (xbox_files.py)
    import xbox_files as X
    with open(iso, 'rb') as f:
        vol = X.find_volume(f)
        if not vol:
            sys.exit('no XDVDFS volume in ' + iso)
        base, root, size = vol
        files = []
        X.walk(f, base, root, size, [], files)
        for p, s, l in files:
            if p.lower() == 'default.xbe':
                f.seek(base + s * X.SECTOR)
                open(out, 'wb').write(f.read(l))
                print('default.xbe', l, 'bytes ->', out)
                return
    sys.exit('no default.xbe')


def paths(xbe_path, out):
    x = XBE(xbe_path)
    found = {}
    for m in PATH_RE.finditer(x.d):
        s = m.group()[:-1].decode('latin1')
        if '\\' not in s and '/' not in s:
            continue
        o = m.start()
        v = x.va(o)
        if v is None:
            continue
        found[v] = dict(path=s, va='0x%08X' % v, section=x.section_of(o), uses=[])
    if found:
        # immediate uses of each string address in code sections
        want = {struct.pack('<I', v): v for v in found}
        for nm, va, vs, ra, rs, fl in x.secs:
            if not (fl & 4):  # executable
                continue
            blob = x.d[ra:ra + rs]
            for k in range(len(blob) - 4):
                b0 = blob[k]
                v = None
                if b0 == 0x68 or 0xB8 <= b0 <= 0xBF:
                    v = want.get(blob[k + 1:k + 5])
                elif b0 == 0xC7 and k + 1 < len(blob):
                    m = blob[k + 1]
                    if m == 0x44 and blob[k + 2] == 0x24:  # mov [esp+d8], imm32
                        v = want.get(blob[k + 4:k + 8])
                    elif 0x40 <= m <= 0x47 and m != 0x44:  # mov [r32+d8], imm32 (the error
                        v = want.get(blob[k + 3:k + 7])    # report's file field, [eax+0x14])
                    elif 0x80 <= m <= 0x87 and m != 0x84:  # mov [r32+d32], imm32
                        v = want.get(blob[k + 6:k + 10])
                if v is not None:
                    found[v]['uses'].append('0x%08X' % (va + k))
    res = sorted(found.values(), key=lambda r: r['path'].lower())
    json.dump(res, open(out, 'w'), indent=1)
    dirs = {}
    for r in res:
        dirs.setdefault(os.path.dirname(r['path'].replace('\\', '/')), []).append(r)
    for d in sorted(dirs):
        print('%-50s %3d paths, %4d uses' % (d, len(dirs[d]), sum(len(r['uses']) for r in dirs[d])))
    print(len(res), 'paths ->', out)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == 'extract':
        extract(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 4 and sys.argv[1] == 'paths':
        paths(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
