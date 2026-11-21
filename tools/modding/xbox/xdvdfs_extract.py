# Minimal XDVDFS (Xbox ISO) extractor: xdvdfs_extract.py <iso> <outdir>
import os, struct, sys
iso, out = sys.argv[1], sys.argv[2]
f = open(iso, 'rb')
f.seek(0x10000)
hdr = f.read(0x800)
assert hdr[:20] == b'MICROSOFT*XBOX*MEDIA'
root_sec, root_size = struct.unpack_from('<II', hdr, 20)
count = 0
def walk(sec, size, path, seen):
    global count
    if size == 0 or (sec, path) in seen: return
    seen.add((sec, path))
    f.seek(sec * 2048); tbl = f.read(size)
    stack = [0]
    visited = set()
    while stack:
        off = stack.pop()
        if off in visited or off * 4 + 14 > len(tbl): continue
        visited.add(off)
        p = off * 4
        l, r, s, sz, attr, nl = struct.unpack_from('<HHIIBB', tbl, p)
        if l == 0xFFFF: continue
        name = tbl[p + 14:p + 14 + nl].decode('latin-1')
        if l: stack.append(l)
        if r: stack.append(r)
        full = os.path.join(path, name)
        if attr & 0x10:
            os.makedirs(full, exist_ok=True)
            walk(s, sz, full, seen)
        else:
            f.seek(s * 2048)
            with open(full, 'wb') as o:
                rem = sz
                while rem:
                    d = f.read(min(rem, 1 << 20)); o.write(d); rem -= len(d)
            count += 1
os.makedirs(out, exist_ok=True)
walk(root_sec, root_size, out, set())
print('files', count)
