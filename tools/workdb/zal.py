"""ZDIR (.ZAL) archive lister: entry kinds and UIBuilder script names. Data only."""
import struct, sys, collections

def read_zdir(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'ZDIR' and d[8:12] == b'DIDF', 'not ZDIR'
    count = struct.unpack_from('<I', d, 0x10)[0]
    grid = 0x18; dlen = grid + 8 + 4 * count; ofst = dlen + 8 + 4 * count
    assert d[grid:grid+4] == b'GRID' and d[dlen:dlen+4] == b'DLEN' and d[ofst:ofst+4] == b'OFST'
    out = []
    for i in range(count):
        h = struct.unpack_from('<I', d, grid + 8 + 4 * i)[0]
        w = struct.unpack_from('<I', d, dlen + 8 + 4 * i)[0]
        o = struct.unpack_from('<I', d, ofst + 8 + 4 * i)[0]
        out.append((h, w >> 28, d[o:o + (w & 0x0FFFFFFF)]))
    return out

def kind(data):
    if data[:2] == b'//' :
        head = data[:600].decode('latin1')
        i = head.find('OBJECT "')
        if i >= 0:
            parts = head[i:].split('"')
            return 'script', parts[1] + ' ' + parts[3], head
        return 'text', '', head
    tag = data[:4]
    if all(32 <= c < 127 for c in tag):
        return 'chunk', tag.decode(), ''
    return 'bin', data[:8].hex(), ''

if __name__ == '__main__':
    for path in sys.argv[1:]:
        es = read_zdir(path)
        c = collections.Counter()
        print('==', path, len(es), 'entries')
        for h, fl, data in es:
            k, info, head = kind(data)
            c[(k, info if k == 'chunk' else '')] += 1
            if k == 'script':
                first = head.splitlines()[0][:100]
                print('  script %08x flags %d %8d bytes  %-40s | %s' % (h, fl, len(data), info, first))
        for k, v in sorted(c.items()):
            print('  kind', k, v)
