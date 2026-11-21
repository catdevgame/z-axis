"""Z-Axis IFF-style chunk walker (FORM/FSCN containers, 4-byte tag + u32 size,
little endian). Summarises chunk tags, counts and header sizes so two files of
the same kind can be compared. Data only.

Usage: iff.py <file> [<file> ...]
"""
import collections, struct, sys

CONTAINERS = {b'FORM', b'FSCN', b'TERR', b'TGEO', b'TGIN', b'TXRC', b'UMTL', b'ULIB', b'UAST', b'UFNT',
              b'SDPT', b'FMDL'}


def printable(tag):
    return all(48 <= c < 91 or c == 32 or 97 <= c < 123 for c in tag)


def walk(d, off, end, depth, stats, path, limit=200000):
    n = 0
    while off + 8 <= end and n < limit:
        tag = d[off:off + 4]
        if not printable(tag):
            break
        size = struct.unpack_from('<I', d, off + 4)[0]
        body = off + 8
        if size > end - body:
            stats['bad'][path + '/' + tag.decode('latin1')] += 1
            break
        key = path + '/' + tag.decode('latin1')
        s = stats['tags'][key]
        s[0] += 1
        s[1].add(size if size < 0x200 else -1)
        # A container's first child must be a printable tag with a sane size.
        if size >= 8 and printable(d[body:body + 4]):
            sub = struct.unpack_from('<I', d, body + 4)[0]
            if sub <= size - 8 and depth < 6:
                walk(d, body, body + size, depth + 1, stats, key)
        off = body + size
        n += 1
    return off


def summary(path):
    d = open(path, 'rb').read()
    stats = {'tags': collections.defaultdict(lambda: [0, set()]), 'bad': collections.Counter()}
    walk(d, 0, len(d), 0, stats, '')
    return stats


if __name__ == '__main__':
    sums = [(p, summary(p)) for p in sys.argv[1:]]
    keys = sorted(set(k for _, s in sums for k in s['tags']))
    print('%-46s' % 'chunk path', ' | '.join('%-34s' % p.split('/')[-1] for p, _ in sums))
    for k in keys:
        row = []
        for _, s in sums:
            if k in s['tags']:
                c, sz = s['tags'][k]
                szs = sorted(x for x in sz if x >= 0)
                row.append('%6d x  hdr %s' % (c, (','.join('%x' % x for x in szs[:4]) + ('+var' if -1 in sz else ''))[:22]))
            else:
                row.append('-')
        print('%-46s' % k[:46], ' | '.join('%-34s' % r for r in row))
    for p, s in sums:
        if s['bad']:
            print('unparsed in', p, dict(list(s['bad'].items())[:5]))
