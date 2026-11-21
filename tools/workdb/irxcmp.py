"""Compare two IOP IRX modules: relocation-masked code similarity, per function. Data only."""
import struct, sys, difflib, hashlib

def load(path):
    d = open(path, 'rb').read()
    shoff = struct.unpack_from('<I', d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
    secs = [struct.unpack_from('<10I', d, shoff + i * shentsize) for i in range(shnum)]
    stro = secs[shstrndx][4]
    name = lambda s: d[stro + s[0]: d.index(b'\0', stro + s[0])].decode()
    S = {name(s): s for s in secs}
    t = S['.text']
    text = bytearray(d[t[4]:t[4] + t[5]])
    words = list(struct.unpack('<%dI' % (len(text) // 4), text))
    masked = list(words)
    for nm, s in S.items():
        if s[1] == 9 and nm in ('.rel.text',):
            for i in range(s[5] // 8):
                off, info = struct.unpack_from('<II', d, s[4] + 8 * i)
                typ = info & 0xFF
                k = off // 4
                if k < len(masked):
                    masked[k] &= {4: 0xFC000000, 5: 0xFFFF0000, 6: 0xFFFF0000, 2: 0}.get(typ, 0xFFFFFFFF)
    iop = S.get('.iopmod')
    modname = ''
    if iop:
        b = d[iop[4]:iop[4] + iop[5]]
        ver = struct.unpack_from('<H', b, 0x18)[0]
        modname = '%s v%x.%02x' % (b[0x1a:].split(b'\0')[0].decode('latin1'), ver >> 8, ver & 0xff)
    # functions: split after jr ra + delay slot
    funcs, start = [], 0
    for i, w in enumerate(words):
        if w == 0x03E00008:
            funcs.append((start, i + 2))
            start = i + 2
    if start < len(words):
        funcs.append((start, len(words)))
    return dict(words=words, masked=masked, funcs=funcs, mod=modname, sections={k: (v[3], v[5]) for k, v in S.items()})

a, b = load(sys.argv[1]), load(sys.argv[2])
print('A', sys.argv[1], a['mod'], 'text words', len(a['words']), 'funcs', len(a['funcs']))
print('B', sys.argv[2], b['mod'], 'text words', len(b['words']), 'funcs', len(b['funcs']))
sm = difflib.SequenceMatcher(None, a['masked'], b['masked'], autojunk=False)
print('whole-text masked similarity %.3f' % sm.ratio())
hb = {}
for s, e in b['funcs']:
    hb.setdefault(hashlib.sha1(struct.pack('<%dI' % (e - s), *b['masked'][s:e])).hexdigest(), []).append(s)
same = diff = 0
rows = []
for s, e in a['funcs']:
    h = hashlib.sha1(struct.pack('<%dI' % (e - s), *a['masked'][s:e])).hexdigest()
    if h in hb:
        same += e - s
        continue
    best = (0, None)
    for s2, e2 in b['funcs']:
        r = difflib.SequenceMatcher(None, a['masked'][s:e], b['masked'][s2:e2], autojunk=False)
        if r.quick_ratio() > best[0]:
            q = r.ratio()
            if q > best[0]:
                best = (q, s2)
    diff += e - s
    rows.append((s * 4, (e - s), best[0], best[1] * 4 if best[1] is not None else None))
print('identical function words %d, differing %d' % (same, diff))
for r in rows:
    print('  A func +0x%04x len %4d best B match ratio %.2f at +0x%s' % (r[0], r[1], r[2], '%04x' % r[3] if r[3] is not None else '-'))
